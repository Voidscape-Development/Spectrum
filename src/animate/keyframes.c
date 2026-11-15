#include "keyframes.h"

#include <ctype.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double bounce_out(double s)
{
	const double n1 = 7.5625, d1 = 2.75;
	if (s < 1.0 / d1)
		return n1 * s * s;
	if (s < 2.0 / d1) {
		s -= 1.5 / d1;
		return n1 * s * s + 0.75;
	}
	if (s < 2.5 / d1) {
		s -= 2.25 / d1;
		return n1 * s * s + 0.9375;
	}
	s -= 2.625 / d1;
	return n1 * s * s + 0.984375;
}

double an_ease_apply(int ease, double s)
{
	if (s <= 0.0)
		return 0.0;
	if (s >= 1.0)
		return 1.0;
	switch (ease) {
	case AN_EASE_IN:
		return s * s * s;
	case AN_EASE_OUT: {
		const double r = 1.0 - s;
		return 1.0 - r * r * r;
	}
	case AN_EASE_IN_OUT: {
		if (s < 0.5)
			return 4.0 * s * s * s;
		const double r = -2.0 * s + 2.0;
		return 1.0 - r * r * r * 0.5;
	}
	case AN_EASE_SMOOTH:
		return s * s * (3.0 - 2.0 * s);
	case AN_EASE_STEP:
		return 0.0; // hold the previous value until the keyframe
	case AN_EASE_BACK: {
		const double c1 = 1.70158, c3 = c1 + 1.0, r = s - 1.0;
		return 1.0 + c3 * r * r * r + c1 * r * r;
	}
	case AN_EASE_ELASTIC:
		return pow(2.0, -10.0 * s) * sin((10.0 * s - 0.75) * (2.0 * M_PI / 3.0)) + 1.0;
	case AN_EASE_BOUNCE:
		return bounce_out(s);
	default:
		return s;
	}
}

static const char *ease_names[AN_EASE_COUNT] = {
	"linear", "in", "out", "in-out", "smooth", "step", "back", "elastic", "bounce",
};

static const char *ease_text_keys[AN_EASE_COUNT] = {
	"Animate.Ease.Linear", "Animate.Ease.In",      "Animate.Ease.Out",
	"Animate.Ease.InOut",  "Animate.Ease.Smooth",  "Animate.Ease.Step",
	"Animate.Ease.Back",   "Animate.Ease.Elastic", "Animate.Ease.Bounce",
};

const char *an_ease_name(int ease)
{
	return ease >= 0 && ease < AN_EASE_COUNT ? ease_names[ease] : ease_names[0];
}

const char *an_ease_text_key(int ease)
{
	return ease >= 0 && ease < AN_EASE_COUNT ? ease_text_keys[ease] : ease_text_keys[0];
}

// Looks up an easing word, ignoring case, '-', '_' and an "ease" prefix
// ("ease-in-out", "EaseInOut" and "inout" are all AN_EASE_IN_OUT).
static int parse_ease(const char *word, size_t len)
{
	char buf[32];
	size_t n = 0;
	for (size_t i = 0; i < len && n + 1 < sizeof(buf); i++) {
		const char c = word[i];
		if (c == '-' || c == '_')
			continue;
		buf[n++] = (char)tolower((unsigned char)c);
	}
	buf[n] = '\0';
	const char *w = buf;
	if (strncmp(w, "ease", 4) == 0 && w[4])
		w += 4;
	static const struct {
		const char *name;
		int ease;
	} aliases[] = {
		{"linear", AN_EASE_LINEAR},     {"lin", AN_EASE_LINEAR},     {"in", AN_EASE_IN},
		{"out", AN_EASE_OUT},           {"inout", AN_EASE_IN_OUT},   {"smooth", AN_EASE_SMOOTH},
		{"smoothstep", AN_EASE_SMOOTH}, {"step", AN_EASE_STEP},      {"hold", AN_EASE_STEP},
		{"back", AN_EASE_BACK},         {"overshoot", AN_EASE_BACK}, {"elastic", AN_EASE_ELASTIC},
		{"bounce", AN_EASE_BOUNCE},
	};
	for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
		if (strcmp(w, aliases[i].name) == 0)
			return aliases[i].ease;
	return -1;
}

static bool is_field_sep(char c)
{
	return c == ' ' || c == '\t' || c == ':' || c == '=' || c == ',' || c == '\r';
}

// Parses one keyframe from [p, end). Returns false if it isn't one.
static bool parse_key(const char *p, const char *end, struct an_keyframe *key)
{
	char buf[256];
	size_t len = (size_t)(end - p);
	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	memcpy(buf, p, len);
	buf[len] = '\0';

	char *s = buf, *next;
	while (is_field_sep(*s))
		s++;
	if (!*s || *s == '#')
		return false;
	key->time = strtod(s, &next);
	if (next == s)
		return false;
	s = next;
	if (*s == 's' || *s == 'S')
		s++;
	while (is_field_sep(*s))
		s++;
	key->value = strtod(s, &next);
	if (next == s)
		return false;
	s = next;
	if (*s == '%')
		s++;
	while (is_field_sep(*s))
		s++;
	key->ease = AN_EASE_LINEAR;
	const char *word = s;
	while (*s && (isalpha((unsigned char)*s) || *s == '-' || *s == '_'))
		s++;
	if (s > word) {
		const int e = parse_ease(word, (size_t)(s - word));
		if (e >= 0)
			key->ease = e;
	}
	if (!isfinite(key->time) || !isfinite(key->value))
		return false;
	if (key->time < 0.0)
		key->time = 0.0;
	return true;
}

size_t an_keys_parse(const char *text, struct an_keyframe *keys, size_t max)
{
	size_t count = 0;
	if (!text)
		return 0;
	const char *p = text;
	while (*p && count < max) {
		const char *end = p;
		while (*end && *end != '\n' && *end != ';')
			end++;
		struct an_keyframe key;
		if (parse_key(p, end, &key)) {
			// Insertion sort, stable for keyframes at the same time.
			size_t i = count;
			while (i > 0 && keys[i - 1].time > key.time) {
				keys[i] = keys[i - 1];
				i--;
			}
			keys[i] = key;
			count++;
		}
		p = *end ? end + 1 : end;
	}
	return count;
}

size_t an_keys_format(const struct an_keyframe *keys, size_t count, char *out, size_t size)
{
	size_t total = 0;
	if (size)
		out[0] = '\0';
	for (size_t i = 0; i < count; i++) {
		char line[128];
		int n;
		if (i == 0 || keys[i].ease == AN_EASE_LINEAR)
			n = snprintf(line, sizeof(line), "%s%g %g", i ? "\n" : "", keys[i].time, keys[i].value);
		else
			n = snprintf(line, sizeof(line), "%s%g %g %s", i ? "\n" : "", keys[i].time, keys[i].value,
				     an_ease_name(keys[i].ease));
		if (n < 0)
			continue;
		if (total + (size_t)n < size)
			memcpy(out + total, line, (size_t)n + 1);
		else if (size)
			out[total < size ? total : size - 1] = '\0';
		total += (size_t)n;
	}
	return total;
}

double an_keys_duration(const struct an_keyframe *keys, size_t count)
{
	return count ? keys[count - 1].time : 0.0;
}

double an_keys_eval(const struct an_keyframe *keys, size_t count, double t)
{
	if (!count)
		return 0.0;
	if (t <= keys[0].time)
		return keys[0].value;
	for (size_t i = 1; i < count; i++) {
		if (t < keys[i].time) {
			const double t0 = keys[i - 1].time, t1 = keys[i].time;
			const double s = (t - t0) / (t1 - t0);
			const double e = an_ease_apply(keys[i].ease, s);
			return keys[i - 1].value + (keys[i].value - keys[i - 1].value) * e;
		}
	}
	return keys[count - 1].value;
}
