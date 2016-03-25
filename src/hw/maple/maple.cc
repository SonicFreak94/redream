#include "hw/holly/holly.h"
#include "hw/maple/maple.h"
#include "hw/maple/maple_controller.h"
#include "hw/dreamcast.h"
#include "hw/memory.h"

using namespace re;
using namespace re::hw;
using namespace re::hw::holly;
using namespace re::hw::maple;
using namespace re::hw::sh4;
using namespace re::ui;

Maple::Maple(Dreamcast &dc)
    : Device(dc),
      WindowInterface(this),
      dc_(dc),
      memory_(nullptr),
      holly_(nullptr),
      devices_() {
  // default controller device
  devices_[0] = std::unique_ptr<MapleController>(new MapleController());
}

bool Maple::Init() {
  memory_ = dc_.memory;
  holly_ = dc_.holly;

  MAPLE_REGISTER_W32_DELEGATE(SB_MDST);

  return true;
}

// The controller can be started up by two methods: by software, or by hardware
// in synchronization with the V-BLANK signal. These methods are selected
// through the trigger selection register (SB_MDTSEL).
void Maple::VBlank() {
  uint32_t enabled = holly_->SB_MDEN;
  uint32_t vblank_initiate = holly_->SB_MDTSEL;

  if (enabled && vblank_initiate) {
    StartDMA();
  }

  // TODO maple vblank interrupt?
}

void Maple::OnKeyDown(Keycode key, int16_t value) {
  std::unique_ptr<MapleDevice> &dev = devices_[0];

  if (!dev) {
    return;
  }

  dev->HandleInput(key, value);
}

void Maple::StartDMA() {
  uint32_t start_addr = holly_->SB_MDSTAR;
  MapleTransferDesc desc;
  MapleFrame frame, res;

  do {
    desc.full = memory_->R64(start_addr);
    start_addr += 8;

    // read input
    frame.header.full = memory_->R32(start_addr);
    start_addr += 4;

    for (uint32_t i = 0; i < frame.header.num_words; i++) {
      frame.params[i] = memory_->R32(start_addr);
      start_addr += 4;
    }

    // handle frame and write response
    std::unique_ptr<MapleDevice> &dev = devices_[desc.port];

    if (dev && dev->HandleFrame(frame, res)) {
      memory_->W32(desc.result_addr, res.header.full);
      desc.result_addr += 4;

      for (uint32_t i = 0; i < res.header.num_words; i++) {
        memory_->W32(desc.result_addr, res.params[i]);
        desc.result_addr += 4;
      }
    } else {
      memory_->W32(desc.result_addr, 0xffffffff);
    }
  } while (!desc.last);

  holly_->SB_MDST = 0;
  holly_->RequestInterrupt(HOLLY_INTC_MDEINT);
}

MAPLE_W32_DELEGATE(SB_MDST) {
  uint32_t enabled = holly_->SB_MDEN;
  if (enabled) {
    if (reg.value) {
      StartDMA();
    }
  } else {
    reg.value = 0;
  }
}
