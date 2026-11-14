/* sd_card_filesystem_none.cpp - the SD card that two kinds of homebrew
 * cartridge (the GameDrive and the El Cheapo SD) read their files from.
 * Upstream gives each program its own: the desktop program's reads a folder of
 * the computer, the libretro core's goes through the frontend's file layer.
 * A Chimera core reads nothing outside the project, so this one says there is
 * no card: those cartridges start as they do with none inserted.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sd_card_filesystem.h"

namespace {

class NoSdCard : public SdCardFileSystem
{
public:
	bool IsAvailable() const override { return false; }
	bool IsValidRootPath(const char *) const override { return false; }
	bool GetFileInfo(const char *, bool &, u32 &) override { return false; }
	bool CreateSizedFile(const char *, u32) override { return false; }
	bool OpenFile(const char *, bool &, u32 &) override { return false; }
	void CloseFile() override {}
	s64 ReadFile(u32, void *, u32) override { return -1; }
	bool WriteFile(u32, const void *, u32) override { return false; }
	bool ReadDirectory(const char *, std::vector<SdCardFileSystemEntry> &) override { return false; }
};

} // namespace

SdCardFileSystem *CreateSdCardFileSystem() { return new NoSdCard(); }
