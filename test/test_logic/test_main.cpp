// Host-side Unity tests for src/dial_logic.cpp (pio test -e native)

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "dial_logic.h"

void setUp() {}
void tearDown() {}

// ==================== Temperature ====================

static void test_clamp_temperature_bounds()
{
  TEST_ASSERT_EQUAL_INT(TEMP_MIN_F, clampTemperatureF(TEMP_MIN_F));
  TEST_ASSERT_EQUAL_INT(TEMP_MAX_F, clampTemperatureF(TEMP_MAX_F));
  TEST_ASSERT_EQUAL_INT(55, clampTemperatureF(54));
  TEST_ASSERT_EQUAL_INT(55, clampTemperatureF(0));
  TEST_ASSERT_EQUAL_INT(55, clampTemperatureF(-40));
  TEST_ASSERT_EQUAL_INT(110, clampTemperatureF(111));
  TEST_ASSERT_EQUAL_INT(110, clampTemperatureF(1000));
  TEST_ASSERT_EQUAL_INT(75, clampTemperatureF(75));
}

static void test_valid_setpoint_range()
{
  TEST_ASSERT_TRUE(isValidSetpointF(55));
  TEST_ASSERT_TRUE(isValidSetpointF(110));
  TEST_ASSERT_TRUE(isValidSetpointF(75));
  TEST_ASSERT_FALSE(isValidSetpointF(54));
  TEST_ASSERT_FALSE(isValidSetpointF(111));
  TEST_ASSERT_FALSE(isValidSetpointF(0));
}

static void test_fahrenheit_celsius_conversion()
{
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, fahrenheitToCelsius(32.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, fahrenheitToCelsius(212.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 23.89f, fahrenheitToCelsius(75.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 32.0f, celsiusToFahrenheit(0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 212.0f, celsiusToFahrenheit(100.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -40.0f, celsiusToFahrenheit(-40.0f));
  // Round trip across the whole dial range
  for (int f = TEMP_MIN_F; f <= TEMP_MAX_F; f++)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, (float)f, celsiusToFahrenheit(fahrenheitToCelsius((float)f)));
}

// ==================== Arc geometry ====================

static void test_setpoint_angle_endpoints()
{
  TEST_ASSERT_FLOAT_WITHIN(0.001f, ARC_START, setpointAngle(TEMP_MIN_F));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, ARC_START + ARC_SPAN, setpointAngle(TEMP_MAX_F));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 135.0f, setpointAngle(55));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 405.0f, setpointAngle(110));
}

static void test_setpoint_angle_clamps_outside_range()
{
  TEST_ASSERT_FLOAT_WITHIN(0.001f, ARC_START, setpointAngle(0));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, ARC_START, setpointAngle(54));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, ARC_START + ARC_SPAN, setpointAngle(111));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, ARC_START + ARC_SPAN, setpointAngle(500));
}

static void test_setpoint_angle_is_linear_and_invertible()
{
  // 82.5°F is the midpoint of 55..110: exactly halfway round the arc
  TEST_ASSERT_FLOAT_WITHIN(0.01f, ARC_START + ARC_SPAN / 2.0f, (setpointAngle(82) + setpointAngle(83)) / 2.0f);
  float perDegree = ARC_SPAN / (TEMP_MAX_F - TEMP_MIN_F);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, perDegree, setpointAngle(76) - setpointAngle(75));
  for (int f = TEMP_MIN_F; f <= TEMP_MAX_F; f++)
    TEST_ASSERT_EQUAL_INT(f, angleSetpoint(setpointAngle(f)));
  TEST_ASSERT_EQUAL_INT(TEMP_MIN_F, angleSetpoint(0.0f));
  TEST_ASSERT_EQUAL_INT(TEMP_MAX_F, angleSetpoint(720.0f));
}

// ==================== Colour ====================

static void test_rgb565_packing()
{
  TEST_ASSERT_EQUAL_HEX16(0x0000, rgb565(0, 0, 0));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, rgb565(255, 255, 255));
  TEST_ASSERT_EQUAL_HEX16(0xF800, rgb565(255, 0, 0));
  TEST_ASSERT_EQUAL_HEX16(0x07E0, rgb565(0, 255, 0));
  TEST_ASSERT_EQUAL_HEX16(0x001F, rgb565(0, 0, 255));
  // Low bits are dropped, not rounded
  TEST_ASSERT_EQUAL_HEX16(0x0000, rgb565(7, 3, 7));
}

static void test_day_gradient_hits_the_five_stops()
{
  // Stops at 55, 65, 80, 95, 110°F as a fraction of 55..110
  TEST_ASSERT_EQUAL_HEX16(rgb565(0x3B, 0x8B, 0xFF), arcColor(0.0f, false));
  TEST_ASSERT_EQUAL_HEX16(rgb565(0x35, 0xC4, 0xE0), arcColor(10.0f / 55.0f, false));
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xE8, 0xE4, 0xDC), arcColor(25.0f / 55.0f, false));
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xFF, 0xA5, 0x3C), arcColor(40.0f / 55.0f, false));
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xFF, 0x5A, 0x3C), arcColor(1.0f, false));
  // Percent is clamped
  TEST_ASSERT_EQUAL_HEX16(arcColor(0.0f, false), arcColor(-1.0f, false));
  TEST_ASSERT_EQUAL_HEX16(arcColor(1.0f, false), arcColor(2.0f, false));
}

static void test_day_gradient_interpolates_between_stops()
{
  // Halfway between the 55°F and 65°F stops: each channel halfway too
  uint16_t mid = arcColor(5.0f / 55.0f, false);
  uint16_t expect = rgb565(0x3B + (0x35 - 0x3B) / 2, 0x8B + (0xC4 - 0x8B) / 2, 0xFF + (0xE0 - 0xFF) / 2);
  TEST_ASSERT_EQUAL_HEX16(expect, mid);
}

static void test_night_gradient_is_red_only()
{
  TEST_ASSERT_EQUAL_HEX16(rgb565(0x88, 0, 0), arcColor(0.0f, true));
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xE0, 0, 0), arcColor(1.0f, true));
  for (int i = 0; i <= 10; i++)
  {
    uint16_t c = arcColor(i / 10.0f, true);
    TEST_ASSERT_EQUAL_HEX16(0, c & 0x07FF); // green and blue fields empty
  }
  TEST_ASSERT_TRUE(arcColor(0.5f, true) > arcColor(0.0f, true));
}

static void test_lerp565()
{
  TEST_ASSERT_EQUAL_HEX16(0x1234, lerp565(0x1234, 0xABCD, 0.0f));
  TEST_ASSERT_EQUAL_HEX16(0xABCD, lerp565(0x1234, 0xABCD, 1.0f));
  // Black to white at 50%: 15/31 red, 31/63 green, 15/31 blue (truncating)
  TEST_ASSERT_EQUAL_HEX16((15 << 11) | (31 << 5) | 15, lerp565(0x0000, 0xFFFF, 0.5f));
  TEST_ASSERT_EQUAL_HEX16(0xF800, lerp565(0xF800, 0xF800, 0.3f));
}

// ==================== Detent acceleration ====================

static void test_accel_single_detents_step_one()
{
  DetentAccel a;
  TEST_ASSERT_EQUAL_INT(1, a.note(1000));
  TEST_ASSERT_EQUAL_INT(1, a.note(2000));
  TEST_ASSERT_EQUAL_INT(1, a.note(3000));
  TEST_ASSERT_EQUAL_INT(1, a.note(4000));
}

// A detent compares against the one three detents back, so the fourth
// detent is the first that can accelerate: three prior detents (three
// intervals) must all fit inside ACCEL_WINDOW_MS.
static void test_accel_three_detents_inside_window_step_two()
{
  DetentAccel a;
  TEST_ASSERT_EQUAL_INT(1, a.note(1000));
  TEST_ASSERT_EQUAL_INT(1, a.note(1050));
  TEST_ASSERT_EQUAL_INT(1, a.note(1100)); // history not yet full: never accelerates
  // Fourth detent exactly ACCEL_WINDOW_MS after the first: still a spin
  TEST_ASSERT_EQUAL_INT(2, a.note(1000 + ACCEL_WINDOW_MS));
  // Keeps accelerating while the spin continues
  TEST_ASSERT_EQUAL_INT(2, a.note(1000 + ACCEL_WINDOW_MS + 20));
  // ...and drops back to 1 once the spin pauses
  TEST_ASSERT_EQUAL_INT(1, a.note(5000));
}

static void test_accel_window_edge_one_ms_late()
{
  DetentAccel a;
  a.note(1000);
  a.note(1050);
  a.note(1100);
  TEST_ASSERT_EQUAL_INT(1, a.note(1000 + ACCEL_WINDOW_MS + 1));
}

static void test_accel_reset_forgets_history()
{
  DetentAccel a;
  a.note(1000);
  a.note(1010);
  a.note(1020);
  a.reset();
  TEST_ASSERT_EQUAL_INT(1, a.note(1030));
  TEST_ASSERT_EQUAL_INT(1, a.note(1040));
  TEST_ASSERT_EQUAL_INT(1, a.note(1050));
  TEST_ASSERT_EQUAL_INT(2, a.note(1060));
}

// ==================== OFF stop ====================

static void test_offstop_normal_detents_adjust()
{
  OffStop s;
  TEST_ASSERT_EQUAL_INT(OFFSTOP_ADJUST, s.detent(true, 75, +1));
  TEST_ASSERT_EQUAL_INT(OFFSTOP_ADJUST, s.detent(true, 75, -1));
  TEST_ASSERT_EQUAL_INT(OFFSTOP_ADJUST, s.detent(true, TEMP_MIN_F, +1));
  TEST_ASSERT_EQUAL_INT(OFFSTOP_ADJUST, s.detent(true, TEMP_MIN_F + 1, -1));
  TEST_ASSERT_EQUAL_INT(0, s.accum);
}

static void test_offstop_needs_exactly_off_detents_below_minimum()
{
  OffStop s;
  for (int i = 1; i < OFF_DETENTS; i++)
  {
    TEST_ASSERT_EQUAL_INT(OFFSTOP_COUNTING, s.detent(true, TEMP_MIN_F, -1));
    TEST_ASSERT_EQUAL_INT(i, s.accum);
  }
  TEST_ASSERT_EQUAL_INT(OFFSTOP_TURN_OFF, s.detent(true, TEMP_MIN_F, -1));
  TEST_ASSERT_EQUAL_INT(0, s.accum);
}

static void test_offstop_upward_detent_resets_counter()
{
  OffStop s;
  TEST_ASSERT_EQUAL_INT(OFFSTOP_COUNTING, s.detent(true, TEMP_MIN_F, -1));
  TEST_ASSERT_EQUAL_INT(1, s.accum);
  TEST_ASSERT_EQUAL_INT(OFFSTOP_ADJUST, s.detent(true, TEMP_MIN_F, +1));
  TEST_ASSERT_EQUAL_INT(0, s.accum);
  // Counting starts over: one more down does not turn off
  TEST_ASSERT_EQUAL_INT(OFFSTOP_COUNTING, s.detent(true, TEMP_MIN_F, -1));
}

static void test_offstop_while_off()
{
  OffStop s;
  TEST_ASSERT_EQUAL_INT(OFFSTOP_IGNORE, s.detent(false, TEMP_MIN_F, -1));
  TEST_ASSERT_EQUAL_INT(OFFSTOP_IGNORE, s.detent(false, 75, -1));
  TEST_ASSERT_EQUAL_INT(OFFSTOP_TURN_ON, s.detent(false, 75, +1));
  TEST_ASSERT_EQUAL_INT(OFFSTOP_TURN_ON, s.detent(false, TEMP_MIN_F, +1));
  TEST_ASSERT_EQUAL_INT(0, s.accum);
}

static void test_offstop_reset()
{
  OffStop s;
  s.detent(true, TEMP_MIN_F, -1);
  TEST_ASSERT_EQUAL_INT(1, s.accum);
  s.reset();
  TEST_ASSERT_EQUAL_INT(0, s.accum);
}

// ==================== Loader shimmer ====================

static void test_shimmer_travels_from_span_start_to_end()
{
  const float from = 200.0f, to = 300.0f; // 100° span
  ShimmerParams start = shimmerAt(0, from, to, false);
  TEST_ASSERT_EQUAL_INT((int)((from - ARC_START) * 10.0f), start.glow10);
  TEST_ASSERT_EQUAL_INT(60, start.sigma10);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, start.pulse);

  // Halfway through the 2600 ms travel the ease-in-out sine is at 0.5
  ShimmerParams mid = shimmerAt(1300, from, to, false);
  TEST_ASSERT_EQUAL_INT((int)((from + 50.0f - ARC_START) * 10.0f), mid.glow10);

  // Just before the end of travel the highlight has reached the far end
  ShimmerParams end = shimmerAt(2599, from, to, false);
  TEST_ASSERT_INT_WITHIN(2, (int)((to - ARC_START) * 10.0f), end.glow10);

  // Monotonic: the highlight never moves backwards during travel
  int last = -1;
  for (unsigned long t = 0; t < 2600; t += 100)
  {
    int g = shimmerAt(t, from, to, false).glow10;
    TEST_ASSERT_TRUE(g >= last);
    last = g;
  }
}

static void test_shimmer_rests_after_travel_and_repeats()
{
  const float from = 200.0f, to = 300.0f;
  TEST_ASSERT_EQUAL_INT(SHIMMER_RESTING, shimmerAt(2600, from, to, false).glow10);
  TEST_ASSERT_EQUAL_INT(SHIMMER_RESTING, shimmerAt(2999, from, to, false).glow10);
  // Period is travel + rest = 3000 ms by day
  TEST_ASSERT_EQUAL_INT(shimmerAt(0, from, to, false).glow10, shimmerAt(3000, from, to, false).glow10);
  TEST_ASSERT_EQUAL_INT(shimmerAt(700, from, to, false).glow10, shimmerAt(3700, from, to, false).glow10);
}

static void test_shimmer_night_is_slower_and_wider()
{
  const float from = 200.0f, to = 300.0f;
  ShimmerParams n = shimmerAt(2000, from, to, true);
  TEST_ASSERT_EQUAL_INT(80, n.sigma10);
  TEST_ASSERT_EQUAL_INT((int)((from + 50.0f - ARC_START) * 10.0f), n.glow10); // 4000 ms travel, midpoint
  TEST_ASSERT_TRUE(shimmerAt(3999, from, to, true).glow10 != SHIMMER_RESTING);
  TEST_ASSERT_EQUAL_INT(SHIMMER_RESTING, shimmerAt(4000, from, to, true).glow10);
  TEST_ASSERT_EQUAL_INT(shimmerAt(0, from, to, true).glow10, shimmerAt(4400, from, to, true).glow10);
}

static void test_shimmer_short_span_pulses_instead()
{
  const float from = 200.0f, to = from + SHIMMER_MIN_SPAN_DEG - 0.5f;
  ShimmerParams p0 = shimmerAt(0, from, to, false);
  TEST_ASSERT_EQUAL_INT(-1, p0.glow10);
  TEST_ASSERT_EQUAL_INT(0, p0.sigma10);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, p0.pulse);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, shimmerAt(600, from, to, false).pulse);  // quarter period
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, shimmerAt(1800, from, to, false).pulse); // three quarters
  // Exactly the minimum span still travels
  TEST_ASSERT_TRUE(shimmerAt(0, from, from + SHIMMER_MIN_SPAN_DEG, false).glow10 >= 0);
}

// ==================== Pod status JSON ====================

static void test_status_rest_format_both_sides()
{
  PodStatus s = parsePodStatus(
      "{\"leftSide\":{\"targetTemperature\":78,\"currentTemperature\":72,\"isPowered\":true},"
      "\"rightSide\":{\"targetTemperature\":66,\"currentTemperature\":70,\"isPowered\":false}}");
  TEST_ASSERT_TRUE(s.success);
  TEST_ASSERT_TRUE(s.left.valid);
  TEST_ASSERT_EQUAL_INT(78, s.left.targetTemperatureF);
  TEST_ASSERT_EQUAL_INT(72, s.left.currentTemperatureF);
  TEST_ASSERT_TRUE(s.left.isPowered);
  TEST_ASSERT_TRUE(s.right.valid);
  TEST_ASSERT_EQUAL_INT(66, s.right.targetTemperatureF);
  TEST_ASSERT_EQUAL_INT(70, s.right.currentTemperatureF);
  TEST_ASSERT_FALSE(s.right.isPowered);
}

static void test_status_legacy_format()
{
  PodStatus s = parsePodStatus(
      "{\"left\":{\"targetTemperatureF\":90,\"currentTemperatureF\":85,\"isOn\":false},"
      "\"right\":{\"targetTemperature\":60}}");
  TEST_ASSERT_TRUE(s.success);
  TEST_ASSERT_EQUAL_INT(90, s.left.targetTemperatureF);
  TEST_ASSERT_EQUAL_INT(85, s.left.currentTemperatureF);
  TEST_ASSERT_FALSE(s.left.isPowered);
  TEST_ASSERT_EQUAL_INT(60, s.right.targetTemperatureF);
  TEST_ASSERT_EQUAL_INT(60, s.right.currentTemperatureF); // current falls back to target
  TEST_ASSERT_TRUE(s.right.isPowered);                    // legacy default: on
}

static void test_status_power_flag_aliases()
{
  // REST format also accepts isOn; legacy also accepts isPowered; a legacy
  // side with neither is assumed on
  PodStatus a = parsePodStatus("{\"leftSide\":{\"targetTemperature\":70,\"isOn\":false}}");
  TEST_ASSERT_FALSE(a.left.isPowered);
  PodStatus b = parsePodStatus("{\"left\":{\"targetTemperatureF\":70,\"isPowered\":false},"
                               "\"right\":{\"targetTemperatureF\":70,\"isPowered\":true}}");
  TEST_ASSERT_FALSE(b.left.isPowered);
  TEST_ASSERT_TRUE(b.right.isPowered);
  PodStatus c = parsePodStatus("{\"left\":{\"targetTemperatureF\":70}}");
  TEST_ASSERT_TRUE(c.left.isPowered);
  // A non-bool power flag is ignored, not coerced
  PodStatus d = parsePodStatus("{\"leftSide\":{\"targetTemperature\":70,\"isPowered\":\"no\"}}");
  TEST_ASSERT_TRUE(d.left.isPowered);
}

static void test_status_legacy_right_power_flags()
{
  PodStatus a = parsePodStatus("{\"right\":{\"targetTemperatureF\":70,\"isOn\":false}}");
  TEST_ASSERT_TRUE(a.right.valid);
  TEST_ASSERT_FALSE(a.right.isPowered);
  PodStatus b = parsePodStatus("{\"right\":{\"targetTemperatureF\":70,\"isOn\":true}}");
  TEST_ASSERT_TRUE(b.right.isPowered);
  // When both flags are present isPowered wins in either format
  PodStatus c = parsePodStatus("{\"right\":{\"targetTemperatureF\":70,\"isPowered\":false,\"isOn\":true},"
                               "\"leftSide\":{\"targetTemperature\":70,\"isPowered\":true,\"isOn\":false}}");
  TEST_ASSERT_FALSE(c.right.isPowered);
  TEST_ASSERT_TRUE(c.left.isPowered);
  // A side present in both formats reads the REST one
  PodStatus d = parsePodStatus("{\"rightSide\":{\"targetTemperature\":90},\"right\":{\"targetTemperatureF\":60}}");
  TEST_ASSERT_EQUAL_INT(90, d.right.targetTemperatureF);
}

static void test_status_field_aliases_and_types()
{
  // Legacy accepts the REST field names as a fallback; a non-integer target is ignored
  PodStatus a = parsePodStatus("{\"left\":{\"targetTemperature\":88,\"currentTemperature\":80}}");
  TEST_ASSERT_EQUAL_INT(88, a.left.targetTemperatureF);
  TEST_ASSERT_EQUAL_INT(80, a.left.currentTemperatureF);
  PodStatus b = parsePodStatus("{\"leftSide\":{\"targetTemperature\":\"hot\",\"targetLevel\":\"x\",\"currentTemperature\":72.5}}");
  TEST_ASSERT_EQUAL_INT(TEMP_DEFAULT_F, b.left.targetTemperatureF);
  TEST_ASSERT_EQUAL_INT(TEMP_DEFAULT_F, b.left.currentTemperatureF); // 72.5 is not an int
  // REST has no alias for currentTemperature
  PodStatus c = parsePodStatus("{\"leftSide\":{\"targetTemperature\":80,\"currentTemperatureF\":70}}");
  TEST_ASSERT_EQUAL_INT(80, c.left.currentTemperatureF);
}

static void test_status_missing_fields_use_defaults()
{
  PodStatus s = parsePodStatus("{\"leftSide\":{}}");
  TEST_ASSERT_TRUE(s.success);
  TEST_ASSERT_TRUE(s.left.valid);
  TEST_ASSERT_FALSE(s.right.valid);
  TEST_ASSERT_EQUAL_INT(TEMP_DEFAULT_F, s.left.targetTemperatureF);
  TEST_ASSERT_EQUAL_INT(TEMP_DEFAULT_F, s.left.currentTemperatureF);
  TEST_ASSERT_TRUE(s.left.isPowered); // inferred from target > 0
  TEST_ASSERT_EQUAL_INT(0, s.right.targetTemperatureF);

  // targetLevel is accepted as an alias for targetTemperature
  PodStatus t = parsePodStatus("{\"rightSide\":{\"targetLevel\":100,\"isOn\":true}}");
  TEST_ASSERT_EQUAL_INT(100, t.right.targetTemperatureF);
  TEST_ASSERT_TRUE(t.right.isPowered);
}

static void test_status_no_sides_is_not_success()
{
  PodStatus s = parsePodStatus("{\"foo\":1}");
  TEST_ASSERT_FALSE(s.success);
  TEST_ASSERT_FALSE(s.left.valid);
  TEST_ASSERT_FALSE(s.right.valid);
  // A side that is not an object is ignored
  PodStatus t = parsePodStatus("{\"leftSide\":42,\"rightSide\":\"x\"}");
  TEST_ASSERT_FALSE(t.success);
}

static void test_status_malformed_json()
{
  const char *err = nullptr;
  PodStatus s = parsePodStatus("{\"leftSide\":{\"targetTemperature\":78", &err);
  TEST_ASSERT_FALSE(s.success);
  TEST_ASSERT_FALSE(s.left.valid);
  TEST_ASSERT_NOT_NULL(err);
  err = nullptr;
  TEST_ASSERT_FALSE(parsePodStatus("", &err).success);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_FALSE(parsePodStatus(nullptr).success);
  TEST_ASSERT_FALSE(parsePodStatus("not json at all").success);
  // A good payload leaves the error pointer null
  err = "stale";
  TEST_ASSERT_TRUE(parsePodStatus("{\"leftSide\":{}}", &err).success);
  TEST_ASSERT_NULL(err);
}

static void test_off_side_target_zero_is_not_a_setpoint()
{
  // sleepypod-core reports target 0 for a side that is off
  PodStatus s = parsePodStatus("{\"leftSide\":{\"targetTemperature\":0,\"currentTemperature\":71,\"isPowered\":false}}");
  TEST_ASSERT_TRUE(s.left.valid);
  TEST_ASSERT_FALSE(s.left.isPowered);
  TEST_ASSERT_EQUAL_INT(0, s.left.targetTemperatureF);
  TEST_ASSERT_EQUAL_INT(71, s.left.currentTemperatureF);
  TEST_ASSERT_EQUAL_INT(78, mergeSetpoint(78, s.left)); // keeps the last real setpoint

  // Without an explicit power flag, target 0 also means off
  PodStatus t = parsePodStatus("{\"rightSide\":{\"targetTemperature\":0}}");
  TEST_ASSERT_FALSE(t.right.isPowered);
  TEST_ASSERT_EQUAL_INT(0, t.right.targetTemperatureF);
  TEST_ASSERT_EQUAL_INT(0, t.right.currentTemperatureF); // current falls back to the (zero) target

  // Legacy format too
  PodStatus u = parsePodStatus("{\"left\":{\"targetTemperatureF\":0,\"isOn\":false}}");
  TEST_ASSERT_EQUAL_INT(0, u.left.targetTemperatureF);
  TEST_ASSERT_EQUAL_INT(82, mergeSetpoint(82, u.left));
}

static void test_merge_setpoint_accepts_only_in_range_targets()
{
  SideStatus side = {80, 75, true, true};
  TEST_ASSERT_EQUAL_INT(80, mergeSetpoint(70, side));
  side.targetTemperatureF = TEMP_MIN_F;
  TEST_ASSERT_EQUAL_INT(TEMP_MIN_F, mergeSetpoint(70, side));
  side.targetTemperatureF = TEMP_MAX_F;
  TEST_ASSERT_EQUAL_INT(TEMP_MAX_F, mergeSetpoint(70, side));
  side.targetTemperatureF = TEMP_MAX_F + 1;
  TEST_ASSERT_EQUAL_INT(70, mergeSetpoint(70, side));
  side.targetTemperatureF = TEMP_MIN_F - 1;
  TEST_ASSERT_EQUAL_INT(70, mergeSetpoint(70, side));
  side.targetTemperatureF = 80;
  side.valid = false;
  TEST_ASSERT_EQUAL_INT(70, mergeSetpoint(70, side));
}

// ==================== Pod settings JSON ====================

static void test_settings_full_payload()
{
  PodSettings s = parsePodSettings(
      "{\"sides\":{\"left\":{\"name\":\"Jon\"},\"right\":{\"name\":\"Partner\"}},"
      "\"device\":{\"temperatureUnit\":\"C\",\"rebootDaily\":true,\"rebootTime\":\"04:30\"}}");
  TEST_ASSERT_TRUE(s.success);
  TEST_ASSERT_EQUAL_STRING("Jon", s.leftName);
  TEST_ASSERT_EQUAL_STRING("Partner", s.rightName);
  TEST_ASSERT_EQUAL_STRING("C", s.temperatureUnit);
  TEST_ASSERT_TRUE(s.rebootDaily);
  TEST_ASSERT_EQUAL_STRING("04:30", s.rebootTime);
  TEST_ASSERT_EQUAL_INT(4, parseRebootHour(s.rebootTime, 3));
}

static void test_settings_defaults_when_fields_missing()
{
  PodSettings s = parsePodSettings("{}");
  TEST_ASSERT_TRUE(s.success);
  TEST_ASSERT_EQUAL_STRING("Left", s.leftName);
  TEST_ASSERT_EQUAL_STRING("Right", s.rightName);
  TEST_ASSERT_EQUAL_STRING("F", s.temperatureUnit);
  TEST_ASSERT_FALSE(s.rebootDaily);
  TEST_ASSERT_EQUAL_STRING("03:00", s.rebootTime);

  // Wrong types are ignored, not coerced
  PodSettings t = parsePodSettings("{\"sides\":{\"left\":{\"name\":7}},\"device\":{\"temperatureUnit\":1,\"rebootDaily\":\"yes\"}}");
  TEST_ASSERT_EQUAL_STRING("Left", t.leftName);
  TEST_ASSERT_EQUAL_STRING("F", t.temperatureUnit);
  TEST_ASSERT_FALSE(t.rebootDaily);
}

static void test_settings_unit_f_and_c()
{
  PodSettings f = parsePodSettings("{\"device\":{\"temperatureUnit\":\"F\"}}");
  PodSettings c = parsePodSettings("{\"device\":{\"temperatureUnit\":\"C\"}}");
  TEST_ASSERT_EQUAL_STRING("F", f.temperatureUnit);
  TEST_ASSERT_EQUAL_STRING("C", c.temperatureUnit);
  TEST_ASSERT_TRUE(strcmp(f.temperatureUnit, "F") == 0);
  TEST_ASSERT_FALSE(strcmp(c.temperatureUnit, "F") == 0);
}

static void test_settings_side_names_of_various_lengths()
{
  PodSettings e = parsePodSettings("{\"sides\":{\"left\":{\"name\":\"\"},\"right\":{\"name\":\"R\"}}}");
  TEST_ASSERT_EQUAL_STRING("", e.leftName); // empty is passed through; the caller keeps its old name
  TEST_ASSERT_EQUAL_STRING("R", e.rightName);

  // Exactly the buffer's capacity (31 chars) fits; longer is cut, always terminated
  char json[160];
  char name31[32];
  memset(name31, 'a', 31);
  name31[31] = '\0';
  snprintf(json, sizeof(json), "{\"sides\":{\"left\":{\"name\":\"%s\"},\"right\":{\"name\":\"%sXYZ\"}}}", name31, name31);
  PodSettings l = parsePodSettings(json);
  TEST_ASSERT_EQUAL_STRING(name31, l.leftName);
  TEST_ASSERT_EQUAL_STRING(name31, l.rightName);
  TEST_ASSERT_EQUAL_INT(POD_NAME_MAX - 1, strlen(l.rightName));

  // UTF-8 survives
  PodSettings u = parsePodSettings("{\"sides\":{\"left\":{\"name\":\"Zo\\u00eb\"}}}");
  TEST_ASSERT_EQUAL_STRING("Zo\xC3\xAB", u.leftName);
}

static void test_settings_malformed_json_keeps_defaults()
{
  const char *err = nullptr;
  PodSettings s = parsePodSettings("{\"sides\":{", &err);
  TEST_ASSERT_FALSE(s.success);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_EQUAL_STRING("Left", s.leftName);
  TEST_ASSERT_EQUAL_STRING("F", s.temperatureUnit);
  TEST_ASSERT_FALSE(parsePodSettings("").success);
  TEST_ASSERT_FALSE(parsePodSettings(nullptr).success);
  err = "stale";
  TEST_ASSERT_TRUE(parsePodSettings("{}", &err).success);
  TEST_ASSERT_NULL(err);
  // rebootTime longer than HH:mm is cut to the buffer, still parseable
  PodSettings t = parsePodSettings("{\"device\":{\"rebootTime\":\"04:30:00.000\",\"rebootDaily\":false}}");
  TEST_ASSERT_EQUAL_INT(7, strlen(t.rebootTime));
  TEST_ASSERT_EQUAL_INT(4, parseRebootHour(t.rebootTime, 3));
  TEST_ASSERT_FALSE(t.rebootDaily);
}

static void test_parse_reboot_hour()
{
  TEST_ASSERT_EQUAL_INT(3, parseRebootHour("03:00", 9));
  TEST_ASSERT_EQUAL_INT(23, parseRebootHour("23:30", 9));
  TEST_ASSERT_EQUAL_INT(0, parseRebootHour("00:15", 9));
  TEST_ASSERT_EQUAL_INT(9, parseRebootHour("3:0", 9)); // too short
  TEST_ASSERT_EQUAL_INT(9, parseRebootHour("", 9));
  TEST_ASSERT_EQUAL_INT(9, parseRebootHour(nullptr, 9));
  TEST_ASSERT_EQUAL_INT(0, parseRebootHour("ab:00", 9)); // non-numeric reads as 0, like String::toInt
}

// ==================== Tweens ====================

static void test_tween_eases_between_endpoints()
{
  Tween t;
  tweenStart(t, 0.0f, 100.0f, 200, 1000);
  TEST_ASSERT_TRUE(t.active);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, tweenValue(t, 1000));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, tweenValue(t, 1100)); // symmetric ease: halfway at half time
  float early = tweenValue(t, 1050), late = tweenValue(t, 1150);
  TEST_ASSERT_TRUE(early > 0.0f && early < 25.0f); // slow start
  TEST_ASSERT_TRUE(late > 75.0f && late < 100.0f); // slow finish
  TEST_ASSERT_TRUE(t.active);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, tweenValue(t, 1200)); // done at dur
  TEST_ASSERT_FALSE(t.active);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, tweenValue(t, 5000)); // stays at the end
}

static void test_tween_same_endpoints_is_inactive_and_zero_duration_is_clamped()
{
  Tween t;
  tweenStart(t, 42.0f, 42.0f, 300, 0);
  TEST_ASSERT_FALSE(t.active);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 42.0f, tweenValue(t, 10));
  tweenStart(t, 0.0f, 1.0f, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(1, t.dur);
  TEST_ASSERT_TRUE(t.active);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, tweenValue(t, 1));
  TEST_ASSERT_FALSE(t.active);
}

static void test_easing_curves()
{
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, easeOutCubic(0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.875f, easeOutCubic(0.5f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, easeOutCubic(1.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, easeInOutCubic(0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.032f, easeInOutCubic(0.2f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.5f, easeInOutCubic(0.5f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.968f, easeInOutCubic(0.8f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, easeInOutCubic(1.0f));
}

// ==================== Time & backlight ====================

static void test_night_hour_window_wraps_midnight()
{
  // Default 22..7
  TEST_ASSERT_TRUE(isNightHour(22, NIGHT_START_HOUR, NIGHT_END_HOUR));
  TEST_ASSERT_TRUE(isNightHour(23, NIGHT_START_HOUR, NIGHT_END_HOUR));
  TEST_ASSERT_TRUE(isNightHour(0, NIGHT_START_HOUR, NIGHT_END_HOUR));
  TEST_ASSERT_TRUE(isNightHour(6, NIGHT_START_HOUR, NIGHT_END_HOUR));
  TEST_ASSERT_FALSE(isNightHour(7, NIGHT_START_HOUR, NIGHT_END_HOUR));
  TEST_ASSERT_FALSE(isNightHour(12, NIGHT_START_HOUR, NIGHT_END_HOUR));
  TEST_ASSERT_FALSE(isNightHour(21, NIGHT_START_HOUR, NIGHT_END_HOUR));
  // A window inside one day
  TEST_ASSERT_FALSE(isNightHour(0, 1, 5));
  TEST_ASSERT_TRUE(isNightHour(1, 1, 5));
  TEST_ASSERT_TRUE(isNightHour(4, 1, 5));
  TEST_ASSERT_FALSE(isNightHour(5, 1, 5));
  TEST_ASSERT_FALSE(isNightHour(23, 1, 5));
}

static void test_should_dim_after_timeout_unless_in_settings()
{
  TEST_ASSERT_FALSE(shouldDim(false, DIM_TIMEOUT_MS, false));
  TEST_ASSERT_TRUE(shouldDim(false, DIM_TIMEOUT_MS + 1, false));
  TEST_ASSERT_FALSE(shouldDim(false, DIM_TIMEOUT_NIGHT_MS + 1, false)); // day keeps the longer timeout
  TEST_ASSERT_TRUE(shouldDim(false, DIM_TIMEOUT_NIGHT_MS + 1, true));
  TEST_ASSERT_FALSE(shouldDim(false, DIM_TIMEOUT_NIGHT_MS, true));
  TEST_ASSERT_FALSE(shouldDim(true, DIM_TIMEOUT_MS * 10, false)); // never inside settings
  TEST_ASSERT_FALSE(shouldDim(true, DIM_TIMEOUT_MS * 10, true));
}

static void test_safe_wake_arms_after_delay()
{
  TEST_ASSERT_FALSE(safeWakeArmed(1000, 1000));
  TEST_ASSERT_FALSE(safeWakeArmed(1000 + SAFE_WAKE_ARM_MS - 1, 1000));
  TEST_ASSERT_TRUE(safeWakeArmed(1000 + SAFE_WAKE_ARM_MS, 1000));
  TEST_ASSERT_TRUE(safeWakeArmed(0, 0xFFFFFFFFul - SAFE_WAKE_ARM_MS + 1)); // survives millis() wrap
}

static void test_format_temp_in_both_units()
{
  char buf[16];
  formatTemp(78, true, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("78", buf);
  formatTemp(78, false, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("25.6", buf);
  formatTemp(TEMP_MIN_F, false, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("12.8", buf);
  formatTemp(110, true, buf, 3); // truncated to the buffer, always terminated
  TEST_ASSERT_EQUAL_STRING("11", buf);
}

static void test_wrap_octet()
{
  TEST_ASSERT_EQUAL_UINT8(0, wrapOctet(0));
  TEST_ASSERT_EQUAL_UINT8(255, wrapOctet(255));
  TEST_ASSERT_EQUAL_UINT8(0, wrapOctet(256));
  TEST_ASSERT_EQUAL_UINT8(1, wrapOctet(257));
  TEST_ASSERT_EQUAL_UINT8(255, wrapOctet(-1));
  TEST_ASSERT_EQUAL_UINT8(0, wrapOctet(-256));
  TEST_ASSERT_EQUAL_UINT8(254, wrapOctet(-258));
  TEST_ASSERT_EQUAL_UINT8(88, wrapOctet(88 + 512));
}

int main(int, char **)
{
  UNITY_BEGIN();

  RUN_TEST(test_clamp_temperature_bounds);
  RUN_TEST(test_valid_setpoint_range);
  RUN_TEST(test_fahrenheit_celsius_conversion);

  RUN_TEST(test_setpoint_angle_endpoints);
  RUN_TEST(test_setpoint_angle_clamps_outside_range);
  RUN_TEST(test_setpoint_angle_is_linear_and_invertible);

  RUN_TEST(test_rgb565_packing);
  RUN_TEST(test_day_gradient_hits_the_five_stops);
  RUN_TEST(test_day_gradient_interpolates_between_stops);
  RUN_TEST(test_night_gradient_is_red_only);
  RUN_TEST(test_lerp565);

  RUN_TEST(test_accel_single_detents_step_one);
  RUN_TEST(test_accel_three_detents_inside_window_step_two);
  RUN_TEST(test_accel_window_edge_one_ms_late);
  RUN_TEST(test_accel_reset_forgets_history);

  RUN_TEST(test_offstop_normal_detents_adjust);
  RUN_TEST(test_offstop_needs_exactly_off_detents_below_minimum);
  RUN_TEST(test_offstop_upward_detent_resets_counter);
  RUN_TEST(test_offstop_while_off);
  RUN_TEST(test_offstop_reset);

  RUN_TEST(test_shimmer_travels_from_span_start_to_end);
  RUN_TEST(test_shimmer_rests_after_travel_and_repeats);
  RUN_TEST(test_shimmer_night_is_slower_and_wider);
  RUN_TEST(test_shimmer_short_span_pulses_instead);

  RUN_TEST(test_status_rest_format_both_sides);
  RUN_TEST(test_status_legacy_format);
  RUN_TEST(test_status_power_flag_aliases);
  RUN_TEST(test_status_legacy_right_power_flags);
  RUN_TEST(test_status_field_aliases_and_types);
  RUN_TEST(test_status_missing_fields_use_defaults);
  RUN_TEST(test_status_no_sides_is_not_success);
  RUN_TEST(test_status_malformed_json);
  RUN_TEST(test_off_side_target_zero_is_not_a_setpoint);
  RUN_TEST(test_merge_setpoint_accepts_only_in_range_targets);

  RUN_TEST(test_settings_full_payload);
  RUN_TEST(test_settings_defaults_when_fields_missing);
  RUN_TEST(test_settings_unit_f_and_c);
  RUN_TEST(test_settings_side_names_of_various_lengths);
  RUN_TEST(test_settings_malformed_json_keeps_defaults);
  RUN_TEST(test_parse_reboot_hour);

  RUN_TEST(test_tween_eases_between_endpoints);
  RUN_TEST(test_tween_same_endpoints_is_inactive_and_zero_duration_is_clamped);
  RUN_TEST(test_easing_curves);

  RUN_TEST(test_night_hour_window_wraps_midnight);
  RUN_TEST(test_should_dim_after_timeout_unless_in_settings);
  RUN_TEST(test_safe_wake_arms_after_delay);
  RUN_TEST(test_format_temp_in_both_units);
  RUN_TEST(test_wrap_octet);

  return UNITY_END();
}
