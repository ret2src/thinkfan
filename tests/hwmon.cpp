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

void test_sparse_temperature_indices()
{
	TemporaryDirectory directory;
	directory.add_file("temp5_input");
	directory.add_file("temp1_input");
	directory.add_file("temp2_input");

	const vector<string> paths = lookup_all<SensorDriver>(directory.path());
	CHECK((paths == vector<string>{
		(directory.path() / "temp1_input").string(),
		(directory.path() / "temp2_input").string(),
		(directory.path() / "temp5_input").string()
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

void test_explicit_indices_preserve_order()
{
	TemporaryDirectory directory;
	directory.add_file("temp1_input");
	directory.add_file("temp2_input");

	const vector<string> paths = lookup_all<SensorDriver>(
		directory.path(),
		nullopt,
		nullopt,
		vector<unsigned int>{2, 1}
	);
	CHECK((paths == vector<string>{
		(directory.path() / "temp2_input").string(),
		(directory.path() / "temp1_input").string()
	}));
}

void test_explicit_no_match_recurses_into_hwmon_directory()
{
	TemporaryDirectory directory;
	directory.add_file("hwmon0/temp1_input");
	directory.add_file("hwmon0/temp2_input");

	const vector<string> paths = lookup_all<SensorDriver>(
		directory.path(),
		nullopt,
		nullopt,
		vector<unsigned int>{1, 2}
	);
	CHECK((paths == vector<string>{
		(directory.path() / "hwmon0/temp1_input").string(),
		(directory.path() / "hwmon0/temp2_input").string()
	}));
}

void test_explicit_partial_match_is_order_independent()
{
	TemporaryDirectory directory;
	directory.add_file("temp1_input");

	const string first_order_error = expect_error([&] {
		HwmonInterface<SensorDriver> interface(
			directory.path().string(), nullopt, nullopt, vector<unsigned int>{1, 2}
		);
		interface.lookup();
	});
	CHECK(first_order_error.find("temp2_input") != string::npos);
	CHECK(first_order_error.find("Found only some requested hwmon files") != string::npos);

	const string reverse_order_error = expect_error([&] {
		HwmonInterface<SensorDriver> interface(
			directory.path().string(), nullopt, nullopt, vector<unsigned int>{2, 1}
		);
		interface.lookup();
	});
	CHECK(reverse_order_error.find("temp2_input") != string::npos);
	CHECK(reverse_order_error.find("Found only some requested hwmon files") != string::npos);
}

void test_name_lookup_and_ambiguity_errors()
{
	TemporaryDirectory directory;
	directory.add_file("hwmon0/name", "unrelated\n");
	directory.add_file("hwmon0/temp1_input");
	directory.add_file("hwmon1/name", "coretemp\n");
	directory.add_file("hwmon1/temp2_input");
	directory.add_file("hwmon1/temp1_input");

	const opt<const string> name = string("coretemp");
	const vector<string> paths = lookup_all<SensorDriver>(directory.path(), name);
	CHECK((paths == vector<string>{
		(directory.path() / "hwmon1/temp1_input").string(),
		(directory.path() / "hwmon1/temp2_input").string()
	}));

	const string missing_error = expect_error([&] {
		HwmonInterface<SensorDriver> interface(
			directory.path().string(), opt<const string>{string("missing")}, nullopt, nullopt
		);
		interface.lookup();
	});
	CHECK(missing_error.find("Could not find an hwmon with this name: missing") != string::npos);

	TemporaryDirectory ambiguous_directory;
	ambiguous_directory.add_file("hwmon0/name", "coretemp\n");
	ambiguous_directory.add_file("hwmon1/name", "coretemp\n");
	const string multiple_error = expect_error([&] {
		HwmonInterface<SensorDriver> interface(
			ambiguous_directory.path().string(), name, nullopt, nullopt
		);
		interface.lookup();
	});
	CHECK(multiple_error.find("Found multiple hwmons with this name") != string::npos);
}

void test_model_lookup_and_ambiguity_errors()
{
	TemporaryDirectory directory;
	directory.add_file("hwmon0/model", "unrelated\n");
	directory.add_file("hwmon1/model", "NVMe Composite\n");
	directory.add_file("hwmon1/temp10_input");
	directory.add_file("hwmon1/temp1_input");

	const opt<const string> model = string("NVMe Composite");
	const vector<string> paths = lookup_all<SensorDriver>(directory.path(), nullopt, model);
	CHECK((paths == vector<string>{
		(directory.path() / "hwmon1/temp1_input").string(),
		(directory.path() / "hwmon1/temp10_input").string()
	}));

	const string missing_error = expect_error([&] {
		HwmonInterface<SensorDriver> interface(
			directory.path().string(), nullopt, opt<const string>{string("missing")}, nullopt
		);
		interface.lookup();
	});
	CHECK(missing_error.find("Could not find a hwmon with this model: missing") != string::npos);

	TemporaryDirectory ambiguous_directory;
	ambiguous_directory.add_file("hwmon0/model", "NVMe Composite\n");
	ambiguous_directory.add_file("hwmon1/model", "NVMe Composite\n");
	const string multiple_error = expect_error([&] {
		HwmonInterface<SensorDriver> interface(
			ambiguous_directory.path().string(), nullopt, model, nullopt
		);
		interface.lookup();
	});
	CHECK(multiple_error.find("Found multiple hwmons with this name") != string::npos);
}

} // namespace

int main()
{
	test_numeric_temperature_order_and_exact_matching();
	test_sparse_temperature_indices();
	test_numeric_pwm_order_and_exact_matching();
	test_no_automatic_matches_fail_cleanly();
	test_explicit_indices_preserve_order();
	test_explicit_no_match_recurses_into_hwmon_directory();
	test_explicit_partial_match_is_order_independent();
	test_name_lookup_and_ambiguity_errors();
	test_model_lookup_and_ambiguity_errors();
	return 0;
}
