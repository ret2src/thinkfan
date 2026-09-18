#include "error.h"
#include "fans.h"
#include "hwmon.h"
#include "sensors.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
		string template_path = (filesystem::temp_directory_path() / "thinkfan-hwmon-XXXXXX").string();
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

	void add_file(const string &relative_path, const string &contents = "0\n")
	{
		const filesystem::path file_path = path_ / relative_path;
		filesystem::create_directories(file_path.parent_path());
		std::ofstream file(file_path);
		file << contents;
	}

	void add_directory(const string &relative_path)
	{ filesystem::create_directories(path_ / relative_path); }

private:
	filesystem::path path_;
};

template<class HwmonT>
vector<string> lookup_all(
	const filesystem::path &path,
		opt<const string> name = nullopt,
		opt<const string> model = nullopt,
		opt<vector<unsigned int>> indices = nullopt
)
{
	HwmonInterface<HwmonT> interface(path.string(), name, model, indices);
	vector<string> result;
	const size_t expected_count = indices ? indices->size() : 0;
	if (indices) {
		for (size_t i = 0; i < expected_count; ++i)
			result.push_back(interface.lookup());
	}
	else {
		while (true) {
			try {
				result.push_back(interface.lookup());
			}
			catch (const Bug &) {
				break;
			}
		}
	}
	return result;
}

template<class Fn>
string expect_error(Fn &&fn)
{
	try {
		fn();
	}
	catch (const Error &error) {
		return error.what();
	}
	CHECK(false);
	return {};
}

void test_numeric_temperature_order_and_exact_matching()
{
	TemporaryDirectory directory;
	directory.add_file("temp10_input");
	directory.add_file("temp1_input");
	directory.add_file("temp2_input");
	directory.add_file("temp1_input_extra");
	directory.add_file("temp1_label");
	directory.add_file("temp1_max");
	directory.add_file("name", "unrelated\n");
	directory.add_directory("device");

	const vector<string> paths = lookup_all<SensorDriver>(directory.path());
	CHECK((paths == vector<string>{
		(directory.path() / "temp1_input").string(),
		(directory.path() / "temp2_input").string(),
		(directory.path() / "temp10_input").string()
	}));
}

void test_numeric_pwm_order_and_exact_matching()
{
	TemporaryDirectory directory;
	directory.add_file("pwm10");
	directory.add_file("pwm1");
	directory.add_file("pwm2");
	directory.add_file("pwm1_enable");

	const vector<string> paths = lookup_all<FanDriver>(directory.path());
	CHECK((paths == vector<string>{
		(directory.path() / "pwm1").string(),
		(directory.path() / "pwm2").string(),
		(directory.path() / "pwm10").string()
	}));
}

void test_no_automatic_matches_fail_cleanly()
{
	TemporaryDirectory sensor_directory;
	const string sensor_error = expect_error([&] {
		HwmonInterface<SensorDriver> interface(sensor_directory.path().string(), nullopt, nullopt, nullopt);
		interface.lookup();
	});
	CHECK(sensor_error.find("No matching temp*_input files found") != string::npos);
	CHECK(sensor_error.find("iterator out of bounds") == string::npos);

	TemporaryDirectory fan_directory;
	const string fan_error = expect_error([&] {
		HwmonInterface<FanDriver> interface(fan_directory.path().string(), nullopt, nullopt, nullopt);
		interface.lookup();
	});
	CHECK(fan_error.find("No matching pwm* files found") != string::npos);
	CHECK(fan_error.find("iterator out of bounds") == string::npos);
}

} // namespace

int main()
{
	test_numeric_temperature_order_and_exact_matching();
	test_numeric_pwm_order_and_exact_matching();
	test_no_automatic_matches_fail_cleanly();
	return 0;
}
