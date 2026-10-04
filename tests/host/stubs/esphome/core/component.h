// Minimal stub for host tests.
#pragma once
#include <cstdint>
namespace esphome { namespace setup_priority { const float AFTER_WIFI=250; }
class Component { public: virtual ~Component(){} virtual void setup(){} virtual void loop(){} virtual void dump_config(){} virtual float get_setup_priority() const {return 0;}
 void mark_failed(){failed_=true;} bool is_failed() const {return failed_;} bool failed_=false; }; }
