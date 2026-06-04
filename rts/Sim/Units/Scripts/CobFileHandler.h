/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef COB_FILE_HANDLER_H
#define COB_FILE_HANDLER_H

#include <deque>
#include <string>
#include <vector>

#include "CobFile.h"
#include "System/UnorderedMap.hpp"

class CCobFileHandler
{
public:
	void Init() { cobFileHandles.reserve(256); }
	void Kill() {
		// never explicitly iterated, can simply clear
		cobFileHandles.clear();
		cobFileObjects.clear();
	}

	CCobFile* GetCobFile(const std::string& name);
	CCobFile* ReloadCobFile(const std::string& name);
	const CCobFile* GetScriptFile(const std::string& name) const;
	void SaveMutableCode(std::vector<std::string>& names, std::vector<std::vector<int>>& codes) const;
	void RestoreMutableCode(const std::vector<std::string>& names, const std::vector<std::vector<int>>& codes);

private:
	spring::unordered_map<std::string, size_t> cobFileHandles;
	std::deque<CCobFile> cobFileObjects;
};

extern CCobFileHandler* cobFileHandler;

#endif
