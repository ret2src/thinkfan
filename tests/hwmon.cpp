#include "config.h"
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

	void add_symlink(const string &target, const string &relative_path)
	{ filesystem::create_symlink(target, path_ / relative_path); }

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
	return interface.lookup_all();
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

void test_direct_input_path_is_not_treated_as_a_directory()
{
	TemporaryDirectory directory;
	directory.add_file("temp1_input");
	directory.add_file("pwm1");

	const vector<string> sensor_paths = lookup_all<SensorDriver>(directory.path() / "temp1_input");
	CHECK((sensor_paths == vector<string>{
		(directory.path() / "temp1_input").string()
	}));

	const vector<string> fan_paths = lookup_all<FanDriver>(directory.path() / "pwm1");
	CHECK((fan_paths == vector<string>{
		(directory.path() / "pwm1").string()
	}));
}

#ifdef USE_YAML
void test_yaml_automatic_discovery_expands_sensor_drivers()
{
	TemporaryDirectory directory;
	directory.add_file("temp10_input", "43000\n");
	directory.add_file("temp1_input", "41000\n");
	directory.add_file("pwm1", "0\n");
	directory.add_file("pwm1_enable", "2\n");

	const filesystem::path config_path = directory.path() / "config.yaml";
	directory.add_file("config.yaml",
		"sensors:\n"
		"  - hwmon: " + directory.path().string() + "\n"
		"    correction: [1, 2]\n"
		"fans:\n"
		"  - hwmon: " + directory.path().string() + "\n"
		"    indices: [1]\n"
		"levels:\n"
		"  - speed: 0\n"
		"    upper_limit: [80, 80]\n"
		"  - speed: 128\n"
		"    lower_limit: [70, 70]\n"
		"safety:\n"
		"  emergency_temp: [105, 82]\n"
	);

	std::unique_ptr<const Config> config(Config::read_config({config_path.string()}));
	TemperatureState temperatures(0);
	config->init(temperatures);
	CHECK(config->sensors().size() == 2);
	CHECK(config->num_temps() == 2);
	for (const auto &sensor : config->sensors())
		sensor->read_temps();
	CHECK((temperatures.raw_temps() == vector<int>{41, 43}));
	CHECK((temperatures.temps() == vector<int>{42, 45}));

	directory.add_file("bad-correction.yaml",
		"sensors:\n"
		"  - hwmon: " + directory.path().string() + "\n"
		"    correction: [1]\n"
		"fans:\n"
		"  - hwmon: " + directory.path().string() + "\n"
		"    indices: [1]\n"
		"levels:\n"
		"  - speed: 0\n"
		"    upper_limit: [80, 80]\n"
		"  - speed: 128\n"
		"    lower_limit: [70, 70]\n"
	);
	const string correction_error = expect_error([&] {
		std::unique_ptr<const Config> bad_config(
			Config::read_config({(directory.path() / "bad-correction.yaml").string()})
		);
	});
	CHECK(correction_error.find("correction") != string::npos);
	CHECK(correction_error.find("2") != string::npos);
}

void test_yaml_automatic_discovery_expands_fan_drivers()
{
	TemporaryDirectory directory;
	directory.add_file("temp1_input", "41000\n");
	directory.add_file("pwm1", "0\n");
	directory.add_file("pwm2", "0\n");

	const filesystem::path config_path = directory.path() / "config.yaml";
	directory.add_file("config.yaml",
		"sensors:\n"
		"  - hwmon: " + directory.path().string() + "\n"
		"fans:\n"
		"  - hwmon: " + directory.path().string() + "\n"
		"levels:\n"
		"  - speed: [0, 0]\n"
		"    upper_limit: [80]\n"
		"  - speed: [128, 128]\n"
		"    lower_limit: [70]\n"
	);

	std::unique_ptr<const Config> config(Config::read_config({config_path.string()}));
	CHECK(config->fan_configs().size() == 2);
}

void test_yaml_direct_input_path_creates_one_sensor_driver()
{
	TemporaryDirectory directory;
	directory.add_file("temp1_input", "41000\n");
	directory.add_file("pwm1", "0\n");
	directory.add_file("pwm1_enable", "2\n");

	const filesystem::path config_path = directory.path() / "config.yaml";
	directory.add_file("config.yaml",
		"sensors:\n"
		"  - hwmon: " + (directory.path() / "temp1_input").string() + "\n"
		"    correction: [1]\n"
		"fans:\n"
		"  - hwmon: " + directory.path().string() + "\n"
		"    indices: [1]\n"
		"levels:\n"
		"  - speed: 0\n"
		"    upper_limit: 80\n"
		"  - speed: 128\n"
		"    lower_limit: 70\n"
	);

	std::unique_ptr<const Config> config(Config::read_config({config_path.string()}));
	TemperatureState temperatures(0);
	config->init(temperatures);
	CHECK(config->sensors().size() == 1);
	CHECK(config->num_temps() == 1);
	config->sensors().front()->read_temps();
	CHECK((temperatures.temps() == vector<int>{42}));
}

void test_yaml_hwmon_fan_model_selector_and_keyword_validation()
{
	TemporaryDirectory directory;
	directory.add_file("sensor/temp1_input", "41000\n");
	directory.add_file("candidate/model", "some-model\n");
	directory.add_file("candidate/pwm1", "0\n");
	directory.add_file("candidate/pwm1_enable", "2\n");

	directory.add_file("config.yaml",
		"sensors:\n"
		"  - hwmon: " + (directory.path() / "sensor").string() + "\n"
		"    indices: [1]\n"
		"fans:\n"
		"  - hwmon: " + directory.path().string() + "\n"
		"    model: some-model\n"
		"    indices: [1]\n"
		"levels:\n"
		"  - speed: 0\n"
		"    upper_limit: 80\n"
		"  - speed: 128\n"
		"    lower_limit: 70\n"
	);

	std::unique_ptr<const Config> config(
		Config::read_config({(directory.path() / "config.yaml").string()})
	);
	TemperatureState temperatures(0);
	config->init(temperatures);
	CHECK(config->fan_configs().size() == 1);
	CHECK(config->fan_configs().front()->fan()->path()
		== (directory.path() / "candidate/pwm1").string());

	directory.add_file("unknown-keyword.yaml",
		"sensors:\n"
		"  - hwmon: " + (directory.path() / "sensor").string() + "\n"
		"    indices: [1]\n"
		"fans:\n"
		"  - hwmon: " + directory.path().string() + "\n"
		"    model: some-model\n"
		"    indices: [1]\n"
		"    unknown: true\n"
		"levels:\n"
		"  - speed: 0\n"
		"    upper_limit: 80\n"
		"  - speed: 128\n"
		"    lower_limit: 70\n"
	);
	const string unknown_keyword_error = expect_error([&] {
		std::unique_ptr<const Config> bad_config(
			Config::read_config({(directory.path() / "unknown-keyword.yaml").string()})
		);
	});
	CHECK(unknown_keyword_error.find("Invalid keyword") != string::npos);
}
#endif

void test_explicit_indices_preserve_order()
{
	TemporaryDirectory directory;
	directory.add_file("temp1_input");
	directory.add_file("temp2_input");

	const vector<string> forward_paths = lookup_all<SensorDriver>(
		directory.path(),
		nullopt,
		nullopt,
		vector<unsigned int>{1, 2}
	);
	CHECK((forward_paths == vector<string>{
		(directory.path() / "temp1_input").string(),
		(directory.path() / "temp2_input").string()
	}));

	const vector<string> reverse_paths = lookup_all<SensorDriver>(
		directory.path(),
		nullopt,
		nullopt,
		vector<unsigned int>{2, 1}
	);
	CHECK((reverse_paths == vector<string>{
		(directory.path() / "temp2_input").string(),
		(directory.path() / "temp1_input").string()
	}));
}

void test_explicit_no_match_recurses_into_hwmon_directory()
{
	TemporaryDirectory directory;
	directory.add_file("hwmon0/temp1_input");
	directory.add_file("hwmon0/temp2_input");

	const vector<string> forward_paths = lookup_all<SensorDriver>(
		directory.path(),
		nullopt,
		nullopt,
		vector<unsigned int>{1, 2}
	);
	CHECK((forward_paths == vector<string>{
		(directory.path() / "hwmon0/temp1_input").string(),
		(directory.path() / "hwmon0/temp2_input").string()
	}));

	const vector<string> reverse_paths = lookup_all<SensorDriver>(
		directory.path(),
		nullopt,
		nullopt,
		vector<unsigned int>{2, 1}
	);
	CHECK((reverse_paths == vector<string>{
		(directory.path() / "hwmon0/temp2_input").string(),
		(directory.path() / "hwmon0/temp1_input").string()
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
	CHECK(first_order_error == reverse_order_error);

	TemporaryDirectory larger_directory;
	larger_directory.add_file("temp1_input");
	larger_directory.add_file("temp3_input");
	const string larger_error = expect_error([&] {
		HwmonInterface<SensorDriver> interface(
			larger_directory.path().string(), nullopt, nullopt, vector<unsigned int>{3, 2, 1}
		);
		interface.lookup();
	});
	CHECK(larger_error.find("Found only some requested hwmon files") != string::npos);
	CHECK(larger_error.find("temp2_input") != string::npos);
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

void test_recursive_lookup_inspects_directory_candidates()
{
	TemporaryDirectory target;
	target.add_file("name", "target-name\n");
	target.add_file("model", "target-model\n");
	target.add_file("temp1_input");

	TemporaryDirectory directory;
	directory.add_file("not-a-directory", "not a hwmon\n");
	directory.add_symlink("not-a-directory", "not-a-directory-link");
	directory.add_symlink(target.path().string(), "candidate");

	const vector<string> expected_paths = {
		(directory.path() / "candidate/temp1_input").string()
	};
	CHECK((lookup_all<SensorDriver>(
		directory.path(), opt<const string>{string("target-name")}
	) == expected_paths));
	CHECK((lookup_all<SensorDriver>(
		directory.path(), nullopt, opt<const string>{string("target-model")}
	) == expected_paths));
}

} // namespace

int main()
{
	test_numeric_temperature_order_and_exact_matching();
	test_sparse_temperature_indices();
	test_numeric_pwm_order_and_exact_matching();
	test_no_automatic_matches_fail_cleanly();
	test_direct_input_path_is_not_treated_as_a_directory();
	#ifdef USE_YAML
	test_yaml_automatic_discovery_expands_sensor_drivers();
	test_yaml_automatic_discovery_expands_fan_drivers();
	test_yaml_direct_input_path_creates_one_sensor_driver();
	test_yaml_hwmon_fan_model_selector_and_keyword_validation();
	#endif
	test_explicit_indices_preserve_order();
	test_explicit_no_match_recurses_into_hwmon_directory();
	test_explicit_partial_match_is_order_independent();
	test_name_lookup_and_ambiguity_errors();
	test_model_lookup_and_ambiguity_errors();
	test_recursive_lookup_inspects_directory_candidates();
	return 0;
}
