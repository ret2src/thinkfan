#include "config.h"
#include "fans.h"
#include "sensors.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace thinkfan;

namespace {

void check(bool condition, const char *expression, const char *file, int line)
{
	if (!condition) {
		std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
		std::abort();
	}
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

class FakeFan : public FanDriver {
public:
	FakeFan() : FanDriver(false) {}

	std::vector<std::string> commands;

	void set_speed(const Level &level) override
	{ commands.push_back(level.str()); }

protected:
	void init() override {}
	string lookup() override { return "test-fan"; }
	string type_name() const override { return "test fan"; }
};

class InspectableTpFan : public TpFanDriver {
public:
	using TpFanDriver::TpFanDriver;

	void set_last_watchdog_ping(std::chrono::system_clock::time_point value)
	{ last_watchdog_ping_ = value; }

	std::chrono::system_clock::time_point last_watchdog_ping() const
	{ return last_watchdog_ping_; }
};

std::string first_line(const std::string &path)
{
	std::ifstream file(path);
	std::string line;
	std::getline(file, line);
	return line;
}

using Time = std::chrono::steady_clock::time_point;

struct SimpleFixture {
	std::unique_ptr<FakeFan> fan;
	FakeFan *fan_ptr;
	StepwiseMapping mapping;
	TemperatureState state;
	TemperatureState::Ref ref;

	SimpleFixture(seconds up_delay = seconds(8), seconds down_delay = seconds(45),
		int emergency = 150)
	: fan(std::make_unique<FakeFan>()),
	  fan_ptr(fan.get()),
	  mapping(std::move(fan)),
	  state(1),
	  ref(state.ref(1))
	{
		auto level0 = std::make_unique<SimpleLevel>(0, 0, 80);
		level0->set_delays(up_delay, down_delay);
		mapping.add_level(std::move(level0));
		auto level1 = std::make_unique<SimpleLevel>(1, 70, 100);
		level1->set_delays(up_delay, down_delay);
		mapping.add_level(std::move(level1));
		auto level2 = std::make_unique<SimpleLevel>(2, 90, 120);
		level2->set_delays(up_delay, down_delay);
		mapping.add_level(std::move(level2));
		mapping.set_emergency_limits({emergency});
	}

	void set_temp(int value, int correction = 0)
	{
		ref.restart();
		ref.add_temp(value, correction);
	}

	void init(int value)
	{
		set_temp(value);
		mapping.init_fanspeed(state);
	}

	bool update(int value, Time now, int correction = 0)
	{
		set_temp(value, correction);
		return mapping.set_fanspeed(state, now);
	}
};

void test_upward_dwell_is_continuous()
{
	SimpleFixture fixture;
	fixture.init(75);
	const Time t0{};

	CHECK(!fixture.update(105, t0));
	CHECK(!fixture.update(105, t0 + seconds(4)));
	CHECK(!fixture.update(85, t0 + seconds(5)));
	CHECK(!fixture.update(105, t0 + seconds(6)));
	CHECK(!fixture.update(105, t0 + seconds(13)));
	CHECK(fixture.update(105, t0 + seconds(14)));
	CHECK(fixture.fan_ptr->commands.back() == "level 2");
}

void test_downward_credit_accumulates_and_pauses()
{
	SimpleFixture fixture;
	fixture.init(75);
	const Time t0{};

	CHECK(!fixture.update(69, t0));
	CHECK(!fixture.update(69, t0 + seconds(10)));
	CHECK(!fixture.update(69, t0 + seconds(20)));
	// The preceding 15 seconds add credit, but neutral resets confirmation.
	CHECK(!fixture.update(85, t0 + seconds(35)));
	CHECK(!fixture.update(85, t0 + seconds(50)));
	// A new cool interval must restore confirmation before stepping down.
	CHECK(!fixture.update(69, t0 + seconds(50)));
	CHECK(!fixture.update(69, t0 + seconds(52)));
	CHECK(fixture.update(69, t0 + seconds(60)));
	CHECK(fixture.fan_ptr->commands.back() == "level 0");
}

void test_hot_zone_decays_credit_and_qualifies_upward()
{
	SimpleFixture fixture;
	fixture.init(75);
	const Time t0{};

	CHECK(!fixture.update(69, t0));
	CHECK(!fixture.update(85, t0 + seconds(20)));
	CHECK(!fixture.update(105, t0 + seconds(23)));
	CHECK(!fixture.update(105, t0 + seconds(26)));
	CHECK(!fixture.update(85, t0 + seconds(27)));
	CHECK(fixture.fan_ptr->commands.back() == "level 1");
}

void test_hot_zone_resets_confirmation_and_starts_upward_dwell()
{
	SimpleFixture fixture(seconds(100), seconds(20));
	fixture.init(75);
	const Time t0{};

	CHECK(!fixture.update(69, t0));
	CHECK(!fixture.update(85, t0 + seconds(20)));
	CHECK(!fixture.update(69, t0 + seconds(20)));
	CHECK(!fixture.update(69, t0 + seconds(22)));
	// Confirmation has reached 2/3 seconds, then HOT resets it.
	CHECK(!fixture.update(105, t0 + seconds(22)));
	CHECK(!fixture.update(105, t0 + seconds(25)));
	CHECK(!fixture.update(85, t0 + seconds(25)));
	CHECK(fixture.fan_ptr->commands.back() == "level 1");
}

void test_credit_clamps_and_turbo_requires_reconfirmation()
{
	SimpleFixture fixture(seconds(8), seconds(20));
	fixture.init(75);
	const Time t0{};

	CHECK(!fixture.update(69, t0));
	CHECK(!fixture.update(85, t0 + seconds(20)));
	// Full credit is retained through neutral, but cannot act without confirmation.
	CHECK(!fixture.update(69, t0 + seconds(20)));
	CHECK(!fixture.update(69, t0 + seconds(22)));
	CHECK(!fixture.update(85, t0 + seconds(22)));
	CHECK(!fixture.update(69, t0 + seconds(22)));
	CHECK(!fixture.update(69, t0 + seconds(24)));
	CHECK(fixture.update(69, t0 + seconds(25)));
	CHECK(fixture.fan_ptr->commands.back() == "level 0");

	SimpleFixture spike(seconds(8), seconds(20));
	spike.init(75);
	CHECK(!spike.update(69, t0));
	CHECK(!spike.update(85, t0 + seconds(20)));
	CHECK(!spike.update(105, t0 + seconds(20)));
	CHECK(!spike.update(105, t0 + seconds(23)));
	CHECK(spike.fan_ptr->commands.back() == "level 1");
	CHECK(!spike.update(85, t0 + seconds(23)));
	CHECK(!spike.update(69, t0 + seconds(23)));
	CHECK(!spike.update(69, t0 + seconds(25)));
	CHECK(spike.fan_ptr->commands.back() == "level 1");

	SimpleFixture decay(seconds(100), seconds(20));
	decay.init(75);
	CHECK(!decay.update(69, t0));
	CHECK(!decay.update(85, t0 + seconds(5)));
	CHECK(!decay.update(105, t0 + seconds(5)));
	CHECK(!decay.update(105, t0 + seconds(35)));
	CHECK(!decay.update(85, t0 + seconds(35)));
	CHECK(!decay.update(69, t0 + seconds(35)));
	CHECK(!decay.update(69, t0 + seconds(53)));
	CHECK(decay.fan_ptr->commands.back() == "level 1");
	CHECK(decay.update(69, t0 + seconds(55)));
	CHECK(decay.fan_ptr->commands.back() == "level 0");
}

void test_upward_transition_clears_downward_state()
{
	SimpleFixture fixture(seconds(8), seconds(20));
	fixture.init(75);
	const Time t0{};

	CHECK(!fixture.update(69, t0));
	CHECK(!fixture.update(85, t0 + seconds(10)));
	CHECK(!fixture.update(105, t0 + seconds(10)));
	CHECK(fixture.update(105, t0 + seconds(18)));
	CHECK(fixture.fan_ptr->commands.back() == "level 2");
	// Level 2 starts with no inherited credit or confirmation.
	CHECK(!fixture.update(85, t0 + seconds(18)));
	CHECK(!fixture.update(85, t0 + seconds(20)));
	CHECK(fixture.fan_ptr->commands.back() == "level 2");
}

void test_emergency_discards_temporal_state()
{
	SimpleFixture fixture(seconds(8), seconds(20), 98);
	fixture.init(75);
	const Time t0{};

	CHECK(!fixture.update(69, t0));
	CHECK(!fixture.update(85, t0 + seconds(10)));
	CHECK(fixture.update(99, t0 + seconds(11), -5));
	CHECK(fixture.state.raw_temps().front() == 99);
	CHECK(fixture.fan_ptr->commands.back() == "level 2");
	CHECK(!fixture.update(75, t0 + seconds(12)));
	CHECK(fixture.fan_ptr->commands.back() == "level 2");
	CHECK(!fixture.update(75, t0 + seconds(31)));
	CHECK(fixture.fan_ptr->commands.back() == "level 2");
}

void test_long_gap_resets_temporal_state()
{
	SimpleFixture fixture(seconds(8), seconds(20));
	fixture.init(75);
	const Time t0{};

	CHECK(!fixture.update(69, t0));
	CHECK(!fixture.update(69, t0 + seconds(10)));
	CHECK(!fixture.update(69, t0 + seconds(100)));
	CHECK(fixture.fan_ptr->commands.back() == "level 1");
	CHECK(!fixture.update(69, t0 + seconds(102)));
	CHECK(fixture.fan_ptr->commands.back() == "level 1");
}

void test_zero_delay_is_immediate()
{
	SimpleFixture fixture(seconds(0), seconds(0));
	fixture.init(75);
	const Time t0{};

	CHECK(fixture.update(69, t0));
	CHECK(fixture.fan_ptr->commands.back() == "level 0");

	SimpleFixture upward(seconds(0), seconds(20));
	upward.init(75);
	CHECK(upward.update(105, t0));
	CHECK(upward.fan_ptr->commands.back() == "level 2");
}

void test_legacy_jump()
{
	auto fan = std::make_unique<FakeFan>();
	FakeFan *fan_ptr = fan.get();
	StepwiseMapping mapping(std::move(fan));
	for (int i = 0; i < 3; ++i)
		mapping.add_level(std::make_unique<SimpleLevel>(i, i == 0 ? 0 : 50 + i * 10,
			i == 2 ? 120 : 50 + (i + 1) * 10));

	TemperatureState state(1);
	auto ref = state.ref(1);
	ref.restart();
	ref.add_temp(40);
	mapping.init_fanspeed(state);
	ref.restart();
	ref.add_temp(110);
	CHECK(mapping.set_fanspeed(state, Time{}));
	CHECK(fan_ptr->commands.back() == "level 2");
}

void test_complex_levels_preserve_zone_semantics()
{
	auto fan = std::make_unique<FakeFan>();
	FakeFan *fan_ptr = fan.get();
	StepwiseMapping mapping(std::move(fan));
	auto level0 = std::make_unique<ComplexLevel>(0, vector<int>{0, 0}, vector<int>{80, 90});
	level0->set_delays(seconds(3), seconds(5));
	mapping.add_level(std::move(level0));
	auto level1 = std::make_unique<ComplexLevel>(1, vector<int>{70, 75}, vector<int>{100, 110});
	level1->set_delays(seconds(3), seconds(5));
	mapping.add_level(std::move(level1));
	mapping.set_emergency_limits({150, 150});

	TemperatureState state(2);
	auto ref = state.ref(2);
	ref.restart();
	ref.add_temp(75);
	ref.add_temp(80);
	mapping.init_fanspeed(state);
	const Time t0{};
	ref.restart();
	ref.add_temp(69);
	ref.add_temp(74);
	CHECK(!mapping.set_fanspeed(state, t0));
	ref.restart();
	ref.add_temp(69);
	ref.add_temp(74);
	CHECK(mapping.set_fanspeed(state, t0 + seconds(5)));
	CHECK(fan_ptr->commands.back() == "level 0");
}

void test_tpacpi_watchdog_refresh()
{
	const std::string path = "/tmp/thinkfan-tpacpi-watchdog-test";
	{
		std::ofstream fan(path);
		fan << "level: 0\n"
			<< "commands: level <level> watchdog <timeout>\n";
	}

	const auto previous_sleeptime = sleeptime;
	sleeptime = seconds(0);
	{
		InspectableTpFan fan(path);
		fan.try_init();
		fan.set_watchdog(7);
		SimpleLevel level(0, 0, 80);

		const auto before_speed_change = std::chrono::system_clock::now();
		fan.set_speed(level);
		CHECK(first_line(path) == "level 0");
		CHECK(fan.last_watchdog_ping() >= before_speed_change);

		const auto stale_ping = std::chrono::system_clock::now() - seconds(8);
		fan.set_last_watchdog_ping(stale_ping);
		fan.ping_watchdog_and_depulse(level);
		CHECK(first_line(path) == "watchdog 7");
		const auto watchdog_ping = fan.last_watchdog_ping();
		CHECK(watchdog_ping > stale_ping);

		fan.ping_watchdog_and_depulse(level);
		CHECK(first_line(path) == "watchdog 7");
		CHECK(fan.last_watchdog_ping() == watchdog_ping);
	}
	sleeptime = previous_sleeptime;
	std::remove(path.c_str());
}

#ifdef USE_YAML
void test_yaml_duration_syntax()
{
	const std::string path = "/tmp/thinkfan-temporal-config.yaml";
	{
		std::ofstream config(path);
		config << "sensors:\n"
			<< "  - hwmon: /tmp/thinkfan-temp\n"
			<< "    indices: [1]\n"
			<< "fans:\n"
			<< "  - hwmon: /tmp/thinkfan-fan\n"
			<< "    indices: [1]\n"
			<< "levels:\n"
			<< "  - speed: 0\n"
			<< "    upper_limit: 80\n"
			<< "    up_delay: 5s\n"
			<< "  - speed: 255\n"
			<< "    lower_limit: 70\n"
			<< "    down_delay: 60s\n"
			<< "safety:\n"
			<< "  emergency_temp: 98\n";
	}

	std::unique_ptr<const Config> config(Config::read_config({path}));
	config->ensure_consistency();
	auto mapping = dynamic_cast<const StepwiseMapping *>(config->fan_configs().front().get());
	CHECK(mapping != nullptr);
	CHECK(mapping->levels().front()->up_delay() == seconds(5));
	CHECK(mapping->levels().back()->down_delay() == seconds(60));
	std::remove(path.c_str());
}
#endif

} // namespace

int main()
{
	test_upward_dwell_is_continuous();
	test_downward_credit_accumulates_and_pauses();
	test_hot_zone_decays_credit_and_qualifies_upward();
	test_hot_zone_resets_confirmation_and_starts_upward_dwell();
	test_credit_clamps_and_turbo_requires_reconfirmation();
	test_upward_transition_clears_downward_state();
	test_emergency_discards_temporal_state();
	test_long_gap_resets_temporal_state();
	test_zero_delay_is_immediate();
	test_legacy_jump();
	test_complex_levels_preserve_zone_semantics();
	test_tpacpi_watchdog_refresh();
	#ifdef USE_YAML
	test_yaml_duration_syntax();
	#endif
	return 0;
}
