// Minimal stub for host tests.
#pragma once
#include <vector>
#include <functional>
#include <cstdlib>
namespace esphome {
template<typename T> class CallbackManager;
template<typename... Ts> class CallbackManager<void(Ts...)> { public: void add(std::function<void(Ts...)>&&f){c.push_back(std::move(f));} void call(Ts... a){for(auto&f:c)f(a...);} std::vector<std::function<void(Ts...)>> c; };
template<class T> class RAMAllocator { public: T* allocate(size_t n){return (T*)malloc(n*sizeof(T));} }; }
