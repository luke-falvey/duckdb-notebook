//===----------------------------------------------------------------------===//
// Small string helpers shared by src/core. Header-only; no DuckDB headers.
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace notebook {

//! `s` without leading and trailing whitespace.
inline std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

inline std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

inline std::string Upper(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return s;
}

inline bool Contains(const std::vector<std::string> &values, const std::string &value) {
	return std::find(values.begin(), values.end(), value) != values.end();
}

//! A decimal number as people write one: an optional sign, digits with an optional fraction, and an
//! optional exponent (`-1.5e3`). Not `inf`, `nan` or hexadecimal, which strtod would also take.
inline bool IsDecimalNumber(const std::string &s) {
	size_t i = 0, digits = 0;
	if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
		i++;
	}
	for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); i++) {
		digits++;
	}
	if (i < s.size() && s[i] == '.') {
		for (i++; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); i++) {
			digits++;
		}
	}
	if (digits == 0) {
		return false;
	}
	if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
		i++;
		if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
			i++;
		}
		size_t exponent = 0;
		for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); i++) {
			exponent++;
		}
		if (exponent == 0) {
			return false;
		}
	}
	return i == s.size();
}

//! `text` with every `from` replaced by `to`.
inline std::string ReplaceAll(std::string text, const std::string &from, const std::string &to) {
	size_t pos = 0;
	while ((pos = text.find(from, pos)) != std::string::npos) {
		text.replace(pos, from.size(), to);
		pos += to.size();
	}
	return text;
}

//! Appends `value` unless it is already there, keeping first-seen order.
inline void AddUnique(std::vector<std::string> &values, const std::string &value) {
	if (!Contains(values, value)) {
		values.push_back(value);
	}
}

} // namespace notebook
