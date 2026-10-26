// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef KYTY_COMMON_SINGLETON_H_
#define KYTY_COMMON_SINGLETON_H_

#include <cstdlib>
#include <new>

namespace Common {

template <class T>
class Singleton {
public:
	static T* Instance() {
		// Console Five Pro fix (v0.0.8pro): the old check-then-construct race let
		// two concurrent callers build two instances (one leaked, different
		// pointers handed out). A function-local static is initialized exactly
		// once by the C++11 thread-safe static initialization rules.
		static T* const instance = []() {
			// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
			auto* storage = static_cast<T*>(std::malloc(sizeof(T)));
			new (storage) T;
			return storage;
		}();
		return instance;
	}

	KYTY_CLASS_NO_COPY(Singleton);

protected:
	Singleton();
	~Singleton();
};

} // namespace Common

#endif /* KYTY_COMMON_SINGLETON_H_ */
