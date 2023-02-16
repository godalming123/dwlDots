// somebar - dwl bar
// See LICENSE file for copyright and license details.

#pragma once
#include "common.hpp"

constexpr bool topbar = true;

constexpr int paddingX = 10;
constexpr int paddingY = 3;

// See https://docs.gtk.org/Pango/type_func.FontDescription.from_string.html
constexpr const char* font = "Sans 12";

constexpr ColorScheme colorInactive = {Color(0xFF, 0xFF, 0xFF), Color(0x2e, 0x34, 0x40)};
constexpr ColorScheme colorActive = {Color(0x00, 0x00, 0x00), Color(0x88, 0xc0, 0xd0)};
constexpr const char* termcmd[] = {"wayst", nullptr};

static std::vector<std::string> tagNames = {"1", "2", "3", "4", "5"};

constexpr Button buttons[] = {
	{ClkStatusText,   BTN_RIGHT,  spawn,      {.v = termcmd} },
};
