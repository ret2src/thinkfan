/********************************************************************
 * hwmon.cpp: Helper functionality for sysfs hwmon interface
 * (C) 2022, Victor Mataré
 *
 * this file is part of thinkfan. See thinkfan.c for further information.
 *
 * thinkfan is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * thinkfan is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with thinkfan.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ******************************************************************/

#include "hwmon.h"
#include "message.h"
#include "error.h"

#include <fnmatch.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <limits>
#include <sys/types.h>
#include <sys/stat.h>
#include <filesystem>

namespace thinkfan {

namespace filesystem = std::filesystem;


static opt<unsigned int> parse_indexed_filename(
	const string &filename,
	const string &prefix,
	const string &suffix
)
{
	if (filename.size() <= prefix.size() + suffix.size()
			|| filename.compare(0, prefix.size(), prefix) != 0
			|| filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)
		return nullopt;

	const string index_string = filename.substr(
		prefix.size(), filename.size() - prefix.size() - suffix.size()
	);
	for (unsigned char c : index_string)
		if (!std::isdigit(c))
			return nullopt;

	try {
		size_t end = 0;
		const unsigned long long index = std::stoull(index_string, &end);
		if (end != index_string.size() || index > std::numeric_limits<unsigned int>::max())
			return nullopt;
		return static_cast<unsigned int>(index);
	}
	catch (const std::out_of_range &) {
		return nullopt;
	}
}


static int filter_hwmon_dirs(const struct dirent *entry)
{
	return (entry->d_type == DT_DIR || entry->d_type == DT_LNK)
		&& (!strncmp("hwmon", entry->d_name, 5) || !strcmp("device", entry->d_name));
}


static int filter_subdirs(const struct dirent *entry)
{
	return (entry->d_type & DT_DIR || entry->d_type == DT_LNK)
		&& string(entry->d_name) != "." && string(entry->d_name) != ".."
		&& string(entry->d_name) != "subsystem";
}


template<>
opt<unsigned int> HwmonInterface<SensorDriver>::index_from_filename(const string &filename)
{ return parse_indexed_filename(filename, "temp", "_input"); }

template<>
opt<unsigned int> HwmonInterface<FanDriver>::index_from_filename(const string &filename)
{ return parse_indexed_filename(filename, "pwm", ""); }

template<>
string HwmonInterface<SensorDriver>::driver_file_pattern()
{ return "temp*_input"; }

template<>
string HwmonInterface<FanDriver>::driver_file_pattern()
{ return "pwm*"; }

template<>
int HwmonInterface<SensorDriver>::filter_driver_file(const struct dirent *entry)
{
	return (entry->d_type == DT_REG || entry->d_type == DT_LNK)
		&& HwmonInterface<SensorDriver>::index_from_filename(entry->d_name).has_value()
	;
}

template<>
int HwmonInterface<FanDriver>::filter_driver_file(const struct dirent *entry)
{
	return (entry->d_type == DT_REG || entry->d_type == DT_LNK)
		&& HwmonInterface<FanDriver>::index_from_filename(entry->d_name).has_value()
	;
}


template<int (* filter_fn)(const struct dirent *)>
vector<string> dir_entries(const filesystem::path &dir)
{
	struct dirent **entries;
	int nentries = ::scandir(dir.c_str(), &entries, filter_fn, nullptr);
	if (nentries == -1)
		return {};

	vector<string> rv;
	for (int i = 0; i < nentries; ++i) {
		rv.emplace_back(dir / entries[i]->d_name);
		::free(entries[i]);
	}
	::free(entries);
	return rv;
}


template<class HwmonT>
vector<string> HwmonInterface<HwmonT>::find_files(const string &path, const vector<unsigned int> &indices)
{
	vector<string> rv;
	for (unsigned int idx : indices) {
		const string fpath(path + "/" + filename(idx));
		std::ifstream f(fpath);
		if (f.is_open() && f.good())
			rv.push_back(fpath);
		else
			throw IOerror("Can't find hwmon file: " + fpath, errno);
	}
	return rv;
}

template<>
string HwmonInterface<SensorDriver>::filename(unsigned int index)
{ return "temp" + std::to_string(index) + "_input"; }

template<>
string HwmonInterface<FanDriver>::filename(unsigned int index)
{ return "pwm" + std::to_string(index); }



template<class HwmonT>
HwmonInterface<HwmonT>::HwmonInterface()
{}

template<class HwmonT>
HwmonInterface<HwmonT>::HwmonInterface(const string &base_path, opt<const string> name, opt<const string> model, opt<vector<unsigned int>> indices)
: base_path_(base_path)
, name_(name)
, model_(model)
, indices_(indices)
{}


template<class HwmonT>
vector<string> HwmonInterface<HwmonT>::find_hwmons_by_name(
	const string &path,
	const string &name,
	unsigned char depth
) {
	const unsigned char max_depth = 5;
	vector<string> result;

	ifstream f(path + "/name");
	if (f.is_open() && f.good()) {
		string tmp;
		if ((f >> tmp) && tmp == name) {
			result.push_back(path);
			return result;
		}
	}
	if (depth >= max_depth) {
		return result;  // don't recurse to subdirs
	}

	for (const filesystem::path subdir : dir_entries<filter_subdirs>(path)) {
		struct stat statbuf;
		int err = stat(subdir.c_str(), &statbuf);
		if (err || (statbuf.st_mode & S_IFMT) != S_IFDIR)
			continue;

		auto found = find_hwmons_by_name(subdir, name, depth + 1);
		result.insert(result.end(), found.begin(), found.end());
	}

	return result;
}


template<class HwmonT>
vector<string> HwmonInterface<HwmonT>::find_hwmons_by_model(
	const string &path,
	const string &model,
	unsigned char depth
) {
	const unsigned char max_depth = 5;
	vector<string> result;

	ifstream f(path + "/model");
	if (f.is_open() && f.good()) {
		string tmp;
		if (getline(f, tmp)) {
			tmp = tmp.erase(tmp.find_last_not_of(" \t\n\r\f\v") + 1);
			if (tmp == model) {
				result.push_back(path);
				return result;
			}
		}
	}
	if (depth >= max_depth) {
		return result; // don't recurse to subdirs
	}

	for (const filesystem::path subdir : dir_entries<filter_subdirs>(path)) {
		struct stat statbuf;
		int err = stat(subdir.c_str(), &statbuf);
		if (err || (statbuf.st_mode & S_IFMT) != S_IFDIR)
			continue;

		auto found = find_hwmons_by_model(subdir, model, depth + 1);
		result.insert(result.end(), found.begin(), found.end());
	}

	return result;
}


template<class HwmonT>
vector<string> HwmonInterface<HwmonT>::find_hwmons_by_indices(
	const string &path,
	const vector<unsigned int> &indices,
	unsigned char depth
) {
	constexpr unsigned char max_depth = 3;

	vector<string> filenames;
	for (unsigned int index : indices)
		filenames.push_back(filename(index));
	vector<string> found_paths;
	vector<string> missing_files;
	for (const filesystem::path fname : filenames) {
		const filesystem::path fpath(path + "/" + fname.string());
		std::ifstream f(fpath);
		if (f.is_open() && f.good())
			found_paths.push_back(fpath);
		else
			missing_files.push_back(fname);
	}

	if (!found_paths.empty() && !missing_files.empty()) {
		string missing;
		for (const string &filename : missing_files) {
			if (!missing.empty())
				missing += ", ";
			missing += filename;
		}
		throw DriverInitError(
			"Found only some requested hwmon files in " + path
			+ "; missing: " + missing
		);
	}

	if (missing_files.empty())
		return found_paths;

	if (depth <= max_depth) {
		for (const filesystem::path hwmon_dir : dir_entries<filter_hwmon_dirs>(path)) {
			vector<string> found = HwmonInterface<HwmonT>::find_hwmons_by_indices(
				hwmon_dir,
				indices,
				depth + 1
			);
			if (!found.empty())
				return found;
		}
	}

	if (depth == 0) {
		string requested;
		for (const string &filename : filenames) {
			if (!requested.empty())
				requested += ", ";
			requested += filename;
		}
		throw DriverInitError("Could not find requested files [" + requested + "] in " + path + ".");
	}

	return {};
}


template<class HwmonT>
void HwmonInterface<HwmonT>::resolve_paths()
{
	if (!paths_resolved_) {
		if (!base_path_)
			throw Bug("Can't lookup sensor because it has no base path");

		string path = *base_path_;

		if (name_) {
			vector<string> paths = find_hwmons_by_name(path, name_.value(), 1);
			if (paths.size() != 1) {
				string msg(path + ": ");
				if (paths.size() == 0) {
					msg += "Could not find an hwmon with this name: " + name_.value();
				} else {
					msg += MSG_MULTIPLE_HWMONS_FOUND;
					for (string hwmon_path : paths)
						msg += " " + hwmon_path;
				}
				throw DriverInitError(msg);
			}
			path = paths[0];
		}
		if (model_) {
			vector<string> paths = find_hwmons_by_model(path, model_.value(), 1);
			if (paths.size() != 1) {
				string msg(path + ": ");
				if (paths.size() == 0) {
					msg += "Could not find a hwmon with this model: " + model_.value();
				} else {
					msg += MSG_MULTIPLE_HWMONS_FOUND;
					for (string hwmon_path : paths)
						msg += " " + hwmon_path;
				}
				throw DriverInitError(msg);
			}
			path = paths[0];
		}
		if (indices_) {
			found_paths_ = find_hwmons_by_indices(path, indices_.value(), 0);
			if (found_paths_.size() == 0)
				throw DriverInitError(path + ": " + "Could not find any hwmons in " + path);
		}
		else if (index_from_filename(filesystem::path(path).filename().string())) {
			std::ifstream f(path);
			if (!f.is_open() || !f.good())
				throw DriverInitError("Could not open hwmon input file " + path);
			found_paths_.push_back(path);
		}
		else {
			vector<string> paths = dir_entries<filter_driver_file>(path);
			std::sort(paths.begin(), paths.end(), [](const string &lhs, const string &rhs) {
				const unsigned int lhs_index = HwmonInterface<HwmonT>::index_from_filename(
					filesystem::path(lhs).filename().string()
				).value();
				const unsigned int rhs_index = HwmonInterface<HwmonT>::index_from_filename(
					filesystem::path(rhs).filename().string()
				).value();
				return lhs_index != rhs_index ? lhs_index < rhs_index : lhs < rhs;
			});
			if (paths.empty())
				throw DriverInitError(
					"No matching " + HwmonInterface<HwmonT>::driver_file_pattern()
					+ " files found in " + path
				);
			found_paths_.swap(paths);
		}
		paths_resolved_ = true;
	}
}


template<class HwmonT>
const vector<string> &HwmonInterface<HwmonT>::lookup_all()
{
	resolve_paths();
	return found_paths_;
}


template<class HwmonT>
string HwmonInterface<HwmonT>::lookup()
{
	resolve_paths();
	if (!paths_it_)
		paths_it_.emplace(found_paths_.begin());

	if (*paths_it_ >= found_paths_.end())
		throw Bug(string(__func__) + ": found_paths_ iterator out of bounds");

	return *paths_it_.value()++;
}



template class HwmonInterface<FanDriver>;
template class HwmonInterface<SensorDriver>;





} // namespace thinkfan
