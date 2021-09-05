// ftpd is a server implementation based on the following:
// - RFC  959 (https://tools.ietf.org/html/rfc959)
// - RFC 3659 (https://tools.ietf.org/html/rfc3659)
// - suggested implementation details from https://cr.yp.to/ftp/filesystem.html
//
// Copyright (C) 2021 Michael Theall
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "ftpServer.h"

#include "fs.h"
#include "licenses.h"
#include "log.h"
#include "platform.h"
#include "socket.h"

#include "imgui.h"

#ifdef NDS
#include <dsiwifi9.h>
#endif

#include <arpa/inet.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
using namespace std::chrono_literals;

#ifdef NDS
#define LOCKED(x) x
#else
#define LOCKED(x)                                                                                  \
	do                                                                                             \
	{                                                                                              \
		auto const lock = std::lock_guard (m_lock);                                                \
		x;                                                                                         \
	} while (0)
#endif

namespace
{
/// \brief Application start time
auto const s_startTime = std::time (nullptr);

#ifndef NDS
/// \brief Mutex for s_freeSpace
platform::Mutex s_lock;
#endif

/// \brief Free space string
std::string s_freeSpace;
}

///////////////////////////////////////////////////////////////////////////
FtpServer::~FtpServer ()
{
	m_quit = true;

#ifndef NDS
	m_thread.join ();
#endif
}

FtpServer::FtpServer (UniqueFtpConfig config_) : m_config (std::move (config_)), m_quit (false)
{
#ifndef NDS
	m_thread = platform::Thread (std::bind (&FtpServer::threadFunc, this));
#endif
}

void FtpServer::draw ()
{
#ifdef NDS
	loop ();
#endif

#ifdef CLASSIC
	{
		char port[7];
#ifndef NDS
		auto const lock = std::lock_guard (m_lock);
#endif
		if (m_socket)
			std::sprintf (port, ":%u", m_socket->sockName ().port ());

        static int idk = 0;
        consoleSelect (&g_logConsole);
        //std::printf ("asdf %x\r", idk++);

		consoleSelect (&g_statusConsole);
		/*std::printf ("\x1b[0;0H\x1b[32;1m%s \x1b[36;1m%s%s",
		    STATUS_STRING,
		    m_socket ? m_socket->sockName ().name () : "Waiting on WiFi",
		    m_socket ? port : "");*/

#ifndef NDS
		char timeBuffer[16];
		auto const now = std::time (nullptr);
		std::strftime (timeBuffer, sizeof (timeBuffer), "%H:%M:%S", std::localtime (&now));

		//std::printf (" \x1b[37;1m%s", timeBuffer);
#endif

		//std::fputs ("\x1b[K", stdout);
		//std::fflush (stdout);
	}

	{
#ifndef NDS
		auto const lock = std::lock_guard (s_lock);
#endif
		if (!s_freeSpace.empty ())
		{
			consoleSelect (&g_statusConsole);
			std::printf ("\x1b[0;%uH\x1b[32;1m%s",
			    static_cast<unsigned> (g_statusConsole.windowWidth - s_freeSpace.size () + 1),
			    s_freeSpace.c_str ());
			std::fflush (stdout);
		}
	}

	{
#ifndef NDS
		auto const lock = std::lock_guard (m_lock);
#endif
#if 0
		consoleSelect (&g_sessionConsole);
		std::fputs ("\x1b[2J", stdout);
		for (auto &session : m_sessions)
		{
			session->draw ();
			if (&session != &m_sessions.back ())
				std::fputc ('\n', stdout);
		}
		std::fflush (stdout);
#endif
	}

	drawLog ();
#else
	auto const &io    = ImGui::GetIO ();
	auto const width  = io.DisplaySize.x;
	auto const height = io.DisplaySize.y;

	ImGui::SetNextWindowPos (ImVec2 (0, 0), ImGuiCond_FirstUseEver);
#ifdef _3DS
	// top screen
	ImGui::SetNextWindowSize (ImVec2 (width, height * 0.5f));
#else
	ImGui::SetNextWindowSize (ImVec2 (width, height));
#endif
	{
		char title[64];

		{
			auto const serverLock = std::lock_guard (m_lock);
			std::snprintf (title,
			    sizeof (title),
			    STATUS_STRING " %s###ftpd",
			    m_socket ? m_name.c_str () : "Waiting for WiFi...");
		}

		ImGui::Begin (title,
		    nullptr,
		    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
#ifndef _3DS
		        | ImGuiWindowFlags_MenuBar
#endif
		);
	}

#ifndef _3DS
	showMenu ();
#endif

#ifndef _3DS
	ImGui::BeginChild (
	    "Logs", ImVec2 (0, 0.5f * height), false, ImGuiWindowFlags_HorizontalScrollbar);
#endif
	drawLog ();
#ifndef _3DS
	ImGui::EndChild ();
#endif

#ifdef _3DS
	ImGui::End ();

	// bottom screen
	ImGui::SetNextWindowSize (ImVec2 (width * 0.8f, height * 0.5f));
	ImGui::SetNextWindowPos (ImVec2 (width * 0.1f, height * 0.5f), ImGuiCond_FirstUseEver);
	ImGui::Begin ("Sessions",
	    nullptr,
	    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
	        ImGuiWindowFlags_MenuBar);

	showMenu ();
#else
	ImGui::Separator ();
#endif

	{
		auto const lock = std::lock_guard (m_lock);
		for (auto &session : m_sessions)
			session->draw ();
	}

	ImGui::End ();
#endif
}

UniqueFtpServer FtpServer::create ()
{
	updateFreeSpace ();

	auto config = FtpConfig::load (FTPDCONFIG);

	return UniqueFtpServer (new FtpServer (std::move (config)));
}

std::string FtpServer::getFreeSpace ()
{
#ifndef NDS
	auto const lock = std::lock_guard (s_lock);
#endif
	return s_freeSpace;
}

void FtpServer::updateFreeSpace ()
{
	struct statvfs st;
#if defined(NDS) || defined(_3DS) || defined(__SWITCH__)
	if (::statvfs ("sdmc:/", &st) != 0)
#else
	if (::statvfs ("/", &st) != 0)
#endif
		return;

	auto freeSpace = fs::printSize (static_cast<std::uint64_t> (st.f_bsize) * st.f_bfree);

#ifndef NDS
	auto const lock = std::lock_guard (s_lock);
#endif
	if (freeSpace != s_freeSpace)
		s_freeSpace = std::move (freeSpace);
}

std::time_t FtpServer::startTime ()
{
	return s_startTime;
}

void FtpServer::handleNetworkFound ()
{
	SockAddr addr;
	if (!platform::networkAddress (addr))
		return;

	std::uint16_t port;

	{
#ifndef NDS
		auto const lock = m_config->lockGuard ();
#endif
		port = m_config->port ();
	}

	addr.setPort (port);

	auto socket = Socket::create ();
	if (!socket)
		return;

	if (port != 0 && !socket->setReuseAddress (true))
		return;

	if (!socket->bind (addr))
		return;

	if (!socket->listen (10))
		return;

	auto const &sockName = socket->sockName ();
	auto const name      = sockName.name ();

	m_name.resize (std::strlen (name) + 3 + 5);
	m_name.resize (std::sprintf (m_name.data (), "[%s]:%u", name, sockName.port ()));

	info ("Started server at %s\n", m_name.c_str ());

	LOCKED (m_socket = std::move (socket));
}

void FtpServer::handleNetworkLost ()
{
	{
		// destroy sessions
		std::vector<UniqueFtpSession> sessions;
		LOCKED (sessions = std::move (m_sessions));
	}

	{
		// destroy command socket
		UniqueSocket sock;
		LOCKED (sock = std::move (m_socket));
	}

	info ("Stopped server at %s\n", m_name.c_str ());
}

#ifndef CLASSIC
void FtpServer::showMenu ()
{
	auto const prevShowSettings = m_showSettings;
	auto const prevShowAbout    = m_showAbout;

	if (ImGui::BeginMenuBar ())
	{
#if defined(_3DS) || defined(__SWITCH__)
		if (ImGui::BeginMenu (u8"Menu \xee\x80\x83")) // Y Button
#else
		if (ImGui::BeginMenu ("Menu"))
#endif
		{
			if (ImGui::MenuItem ("Settings"))
				m_showSettings = true;

			if (ImGui::MenuItem ("About"))
				m_showAbout = true;

			ImGui::EndMenu ();
		}
		ImGui::EndMenuBar ();
	}

	if (m_showSettings)
	{
		if (!prevShowSettings)
		{
#ifndef NDS
			auto const lock = m_config->lockGuard ();
#endif

			m_userSetting = m_config->user ();
			m_userSetting.resize (32);

			m_passSetting = m_config->pass ();
			m_passSetting.resize (32);

			m_portSetting = m_config->port ();

#ifdef _3DS
			m_getMTimeSetting = m_config->getMTime ();
#endif

#ifdef __SWITCH__
			m_enableAPSetting = m_config->enableAP ();

			m_ssidSetting = m_config->ssid ();
			m_ssidSetting.resize (19);

			m_passphraseSetting = m_config->passphrase ();
			m_passphraseSetting.resize (63);
#endif

			ImGui::OpenPopup ("Settings");
		}

		showSettings ();
	}

	if (m_showAbout)
	{
		if (!prevShowAbout)
			ImGui::OpenPopup ("About");

		showAbout ();
	}
}

void FtpServer::showSettings ()
{
#ifdef _3DS
	auto const &io    = ImGui::GetIO ();
	auto const width  = io.DisplaySize.x;
	auto const height = io.DisplaySize.y;

	ImGui::SetNextWindowSize (ImVec2 (width * 0.8f, height * 0.5f));
	ImGui::SetNextWindowPos (ImVec2 (width * 0.1f, height * 0.5f));
	if (ImGui::BeginPopupModal ("Settings",
	        nullptr,
	        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
#else
	if (ImGui::BeginPopupModal ("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
#endif
	{
		ImGui::InputText ("User",
		    m_userSetting.data (),
		    m_userSetting.size (),
		    ImGuiInputTextFlags_AutoSelectAll);

		ImGui::InputText ("Pass",
		    m_passSetting.data (),
		    m_passSetting.size (),
		    ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_Password);

		ImGui::InputScalar ("Port",
		    ImGuiDataType_U16,
		    &m_portSetting,
		    nullptr,
		    nullptr,
		    "%u",
		    ImGuiInputTextFlags_AutoSelectAll);

#ifdef _3DS
		ImGui::Checkbox ("Get mtime", &m_getMTimeSetting);
#endif

#ifdef __SWITCH__
		ImGui::Checkbox ("Enable Access Point", &m_enableAPSetting);

		ImGui::InputText ("SSID",
		    m_ssidSetting.data (),
		    m_ssidSetting.size (),
		    ImGuiInputTextFlags_AutoSelectAll);
		auto const ssidError = platform::validateSSID (m_ssidSetting);
		if (ssidError)
			ImGui::TextColored (ImVec4 (1.0f, 0.4f, 0.4f, 1.0f), ssidError);

		ImGui::InputText ("Passphrase",
		    m_passphraseSetting.data (),
		    m_passphraseSetting.size (),
		    ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_Password);
		auto const passphraseError = platform::validatePassphrase (m_passphraseSetting);
		if (passphraseError)
			ImGui::TextColored (ImVec4 (1.0f, 0.4f, 0.4f, 1.0f), passphraseError);
#endif

		static ImVec2 const sizes[] = {
		    ImGui::CalcTextSize ("Apply"),
		    ImGui::CalcTextSize ("Save"),
		    ImGui::CalcTextSize ("Reset"),
		    ImGui::CalcTextSize ("Cancel"),
		};

		static auto const maxWidth = std::max_element (
		    std::begin (sizes), std::end (sizes), [] (auto const &lhs_, auto const &rhs_) {
			    return lhs_.x < rhs_.x;
		    })->x;

		static auto const maxHeight = std::max_element (
		    std::begin (sizes), std::end (sizes), [] (auto const &lhs_, auto const &rhs_) {
			    return lhs_.y < rhs_.y;
		    })->y;

		auto const &style = ImGui::GetStyle ();
		auto const width  = maxWidth + 2 * style.FramePadding.x;
		auto const height = maxHeight + 2 * style.FramePadding.y;

		auto const apply = ImGui::Button ("Apply", ImVec2 (width, height));
		ImGui::SameLine ();
		auto const save = ImGui::Button ("Save", ImVec2 (width, height));
		ImGui::SameLine ();
		auto const reset = ImGui::Button ("Reset", ImVec2 (width, height));
		ImGui::SameLine ();
		auto const cancel = ImGui::Button ("Cancel", ImVec2 (width, height));

		if (apply || save)
		{
			m_showSettings = false;
			ImGui::CloseCurrentPopup ();

#ifndef NDS
			auto const lock = m_config->lockGuard ();
#endif

			m_config->setUser (m_userSetting);
			m_config->setPass (m_passSetting);
			m_config->setPort (m_portSetting);

#ifdef _3DS
			m_config->setGetMTime (m_getMTimeSetting);
#endif

#ifdef __SWITCH__
			m_config->setEnableAP (m_enableAPSetting);
			m_config->setSSID (m_ssidSetting);
			m_config->setPassphrase (m_passphraseSetting);
			m_apError = false;
#endif

			UniqueSocket socket;
			LOCKED (socket = std::move (m_socket));
		}

		if (save)
		{
#ifndef NDS
			auto const lock = m_config->lockGuard ();
#endif
			if (!m_config->save (FTPDCONFIG))
				error ("Failed to save config\n");
		}

		if (reset)
		{
			static auto const defaults = FtpConfig::create ();

			m_userSetting = defaults->user ();
			m_passSetting = defaults->pass ();
			m_portSetting = defaults->port ();
#ifdef _3DS
			m_getMTimeSetting = defaults->getMTime ();
#endif

#ifdef __SWITCH__
			m_enableAPSetting   = defaults->enableAP ();
			m_ssidSetting       = defaults->ssid ();
			m_passphraseSetting = defaults->passphrase ();
#endif
		}

		if (apply || save || cancel)
		{
			m_showSettings = false;
			ImGui::CloseCurrentPopup ();
		}

		ImGui::EndPopup ();
	}
}

void FtpServer::showAbout ()
{
	auto const &io    = ImGui::GetIO ();
	auto const width  = io.DisplaySize.x;
	auto const height = io.DisplaySize.y;

#ifdef _3DS
	ImGui::SetNextWindowSize (ImVec2 (width * 0.8f, height * 0.5f));
	ImGui::SetNextWindowPos (ImVec2 (width * 0.1f, height * 0.5f));
#else
	ImGui::SetNextWindowSize (ImVec2 (width * 0.8f, height * 0.8f));
	ImGui::SetNextWindowPos (ImVec2 (width * 0.1f, height * 0.1f));
#endif
	if (ImGui::BeginPopupModal ("About",
	        nullptr,
	        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
	{
		ImGui::TextUnformatted (STATUS_STRING);
		ImGui::TextWrapped ("Copyright © 2021 Michael Theall, Dave Murphy, TuxSH");
		ImGui::Separator ();
		ImGui::Text ("Platform: %s", io.BackendPlatformName);
		ImGui::Text ("Renderer: %s", io.BackendRendererName);

		if (ImGui::Button ("OK", ImVec2 (100, 0)))
		{
			m_showAbout = false;
			ImGui::CloseCurrentPopup ();
		}

		ImGui::Separator ();
		if (ImGui::TreeNode (g_dearImGuiVersion))
		{
			ImGui::TextWrapped (g_dearImGuiCopyright);
			ImGui::Separator ();
			ImGui::TextWrapped (g_mitLicense);
			ImGui::TreePop ();
		}

#if defined(NDS)
#elif defined(_3DS)
		if (ImGui::TreeNode (g_libctruVersion))
		{
			ImGui::TextWrapped (g_zlibLicense);
			ImGui::Separator ();
			ImGui::TextWrapped (g_zlibLicense);
			ImGui::TreePop ();
		}

		if (ImGui::TreeNode (g_citro3dVersion))
		{
			ImGui::TextWrapped (g_citro3dCopyright);
			ImGui::Separator ();
			ImGui::TextWrapped (g_zlibLicense);
			ImGui::TreePop ();
		}

#elif defined(__SWITCH__)
		if (ImGui::TreeNode (g_libnxVersion))
		{
			ImGui::TextWrapped (g_libnxCopyright);
			ImGui::Separator ();
			ImGui::TextWrapped (g_libnxLicense);
			ImGui::TreePop ();
		}

		if (ImGui::TreeNode (g_deko3dVersion))
		{
			ImGui::TextWrapped (g_deko3dCopyright);
			ImGui::Separator ();
			ImGui::TextWrapped (g_zlibLicense);
			ImGui::TreePop ();
		}

		if (ImGui::TreeNode (g_zstdVersion))
		{
			ImGui::TextWrapped (g_zstdCopyright);
			ImGui::Separator ();
			ImGui::TextWrapped (g_bsdLicense);
			ImGui::TreePop ();
		}
#else
		if (ImGui::TreeNode (g_glfwVersion))
		{
			ImGui::TextWrapped (g_glfwCopyright);
			ImGui::Separator ();
			ImGui::TextWrapped (g_zlibLicense);
			ImGui::TreePop ();
		}
#endif

		ImGui::EndPopup ();
	}
}
#endif

void FtpServer::loop ()
{
	if (!m_socket)
	{
#ifndef CLASSIC
#ifdef __SWITCH__
		if (!m_apError)
		{
			bool enable;
			std::string ssid;
			std::string passphrase;

			{
				auto const lock = m_config->lockGuard ();
				enable          = m_config->enableAP ();
				ssid            = m_config->ssid ();
				passphrase      = m_config->passphrase ();
			}

			m_apError = !platform::enableAP (enable, ssid, passphrase);
		}
#endif
#endif

		if (platform::networkVisible ())
			handleNetworkFound ();
	}

	// poll listen socket
	if (m_socket)
	{
		Socket::PollInfo info{*m_socket, POLLIN, 0};
		auto const rc = Socket::poll (&info, 1, 0ms);
		if (rc < 0)
		{
			handleNetworkLost ();
			return;
		}

		if (rc > 0 && (info.revents & POLLIN))
		{
			auto socket = m_socket->accept ();
			if (socket)
			{
				auto session = FtpSession::create (*m_config, std::move (socket));
				LOCKED (m_sessions.emplace_back (std::move (session)));
			}
			else
			{
				handleNetworkLost ();
				return;
			}
		}
	}

	{
		std::vector<UniqueFtpSession> deadSessions;
		{
			// remove dead sessions
#ifndef NDS
			auto const lock = std::lock_guard (m_lock);
#endif
			auto it = std::begin (m_sessions);
			while (it != std::end (m_sessions))
			{
				auto &session = *it;
				if (session->dead ())
				{
					deadSessions.emplace_back (std::move (session));
					it = m_sessions.erase (it);
				}
				else
					++it;
			}
		}
	}

	// poll sessions
	if (!m_sessions.empty ())
	{
		if (!FtpSession::poll (m_sessions))
			handleNetworkLost ();
	}
#ifndef NDS
	// avoid busy polling in background thread
	else
		platform::Thread::sleep (16ms);
#endif
}

void FtpServer::threadFunc ()
{
	while (!m_quit)
		loop ();
}
