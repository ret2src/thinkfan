#include "error.h"
#include "thinkfan.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace thinkfan;

namespace {

namespace filesystem = std::filesystem;

void check(bool condition, const char *expression, const char *file, int line)
{
	if (!condition) {
		std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
		std::abort();
	}
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

class TemporaryDirectory {
public:
	TemporaryDirectory()
	{
		string template_path = (filesystem::temp_directory_path() / "thinkfan-pidfile-XXXXXX").string();
		vector<char> writable_template(template_path.begin(), template_path.end());
		writable_template.push_back('\0');
		const char *created_path = ::mkdtemp(writable_template.data());
		CHECK(created_path != nullptr);
		path_ = created_path;
	}

	~TemporaryDirectory()
	{ filesystem::remove_all(path_); }

	const filesystem::path &path() const
	{ return path_; }

private:
	filesystem::path path_;
};

void write_pid_file(const filesystem::path &path, const string &contents)
{
	std::ofstream file(path);
	CHECK(file.is_open());
	file << contents;
}

string read_pid_file(const filesystem::path &path)
{
	std::ifstream file(path);
	string contents;
	std::getline(file, contents);
	return contents;
}

pid_t exited_child_pid()
{
	const pid_t child = ::fork();
	CHECK(child >= 0);
	if (child == 0)
		::_exit(0);
	CHECK(::waitpid(child, nullptr, 0) == child);
	return child;
}

void test_missing_pid_file()
{
	TemporaryDirectory directory;
	const filesystem::path path = directory.path() / "thinkfan.pid";
	CHECK(inspect_pid_file(path.string()).state == PidFileState::missing);

	{
		PidFileHolder holder(::getpid(), path.string());
		CHECK(read_pid_file(path) == std::to_string(::getpid()));
	}
	CHECK(!filesystem::exists(path));
}

void test_stale_pid_file_is_recovered()
{
	TemporaryDirectory directory;
	const filesystem::path path = directory.path() / "thinkfan.pid";
	const pid_t stale_pid = exited_child_pid();
	write_pid_file(path, std::to_string(stale_pid));
	CHECK(inspect_pid_file(path.string()).state == PidFileState::stale);

	{
		PidFileHolder holder(::getpid(), path.string());
		CHECK(read_pid_file(path) == std::to_string(::getpid()));
	}
	CHECK(!filesystem::exists(path));
}

void test_live_pid_file_is_preserved_and_rejected()
{
	TemporaryDirectory directory;
	const filesystem::path path = directory.path() / "thinkfan.pid";
	write_pid_file(path, std::to_string(::getpid()));
	CHECK(inspect_pid_file(path.string()).state == PidFileState::live);

	bool rejected = false;
	try {
		PidFileHolder holder(::getpid(), path.string());
	}
	catch (const SystemError &error) {
		rejected = true;
		CHECK(string(error.what()).find(path.string()) != string::npos);
	}
	CHECK(rejected);
	CHECK(filesystem::exists(path));
	CHECK(read_pid_file(path) == std::to_string(::getpid()));
}

void test_malformed_pid_files_are_recovered()
{
	for (const string &contents : {string(), string("abc"), string("-1")}) {
		TemporaryDirectory directory;
		const filesystem::path path = directory.path() / "thinkfan.pid";
		write_pid_file(path, contents);
		CHECK(inspect_pid_file(path.string()).state == PidFileState::malformed);

		{
			PidFileHolder holder(::getpid(), path.string());
			CHECK(read_pid_file(path) == std::to_string(::getpid()));
		}
		CHECK(!filesystem::exists(path));
	}
}

} // namespace

int main()
{
	test_missing_pid_file();
	test_stale_pid_file_is_recovered();
	test_live_pid_file_is_preserved_and_rejected();
	test_malformed_pid_files_are_recovered();
	return 0;
}
