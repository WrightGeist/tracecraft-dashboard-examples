// The encoder is decoded in an interrupt, because polling misses fast turns. A table of
// valid quadrature transitions rejects contact bounce. Pins: A = GPIO45, B = GPIO42,
// dial button = GPIO41 (low when pressed).
#include "Dialpad.h"
#include "tracecraft/tc_hal.h"   // hal::millis
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

namespace {

constexpr gpio_num_t PIN_A  = GPIO_NUM_45;
constexpr gpio_num_t PIN_B  = GPIO_NUM_42;
constexpr gpio_num_t PIN_SW = GPIO_NUM_41;

portMUX_TYPE     s_mux      = portMUX_INITIALIZER_UNLOCKED;
volatile int16_t s_encDelta = 0;   // detents counted by the interrupt, taken each poll

// One quadrature step per edge. The last two full steps, as an 8-bit history, read
// 0x2b turning left and 0x17 turning right.
void IRAM_ATTR encISR(void*) {
  static const int8_t kValid[16] = { 0,1,1,0, 1,0,0,1, 1,0,0,1, 0,1,1,0 };
  static uint8_t  prevNext = 0;
  static uint16_t store    = 0;
  prevNext <<= 2;
  if (gpio_get_level(PIN_B)) prevNext |= 0x02;
  if (gpio_get_level(PIN_A)) prevNext |= 0x01;
  prevNext &= 0x0f;
  if (kValid[prevNext]) {
    store = (uint16_t)((store << 4) | prevNext);
    portENTER_CRITICAL_ISR(&s_mux);
    if ((store & 0xff) == 0x2b)      s_encDelta = s_encDelta - 1;
    else if ((store & 0xff) == 0x17) s_encDelta = s_encDelta + 1;
    portEXIT_CRITICAL_ISR(&s_mux);
  }
}

// A detent is a moment; the button stays down this long so the sim sees the press.
constexpr uint32_t kPulseMs = 120;
uint32_t s_leftUntil = 0, s_rightUntil = 0;
uint8_t  s_leftBtn = 0, s_rightBtn = 1;   // the button each direction's press is on
uint8_t  s_page = 0;

int      s_swPrev = 1;
uint32_t s_swLastMs = 0;

} // namespace

void dialpad_set_page(uint8_t page) { s_page = page > 2 ? 0 : page; }

bool dialpad_take_press() {
  const uint32_t now = tracecraft::hal::millis();
  const int sw = gpio_get_level(PIN_SW);
  const bool edge = (s_swPrev == 1 && sw == 0 && (now - s_swLastMs) > 200);
  s_swPrev = sw;
  if (edge) s_swLastMs = now;
  return edge;
}

void dialpad_begin() {
  gpio_config_t ab = {};
  ab.pin_bit_mask = (1ULL << PIN_A) | (1ULL << PIN_B);
  ab.mode         = GPIO_MODE_INPUT;
  ab.pull_up_en   = GPIO_PULLUP_ENABLE;
  ab.intr_type    = GPIO_INTR_ANYEDGE;
  gpio_config(&ab);

  gpio_config_t sw = {};
  sw.pin_bit_mask = (1ULL << PIN_SW);
  sw.mode         = GPIO_MODE_INPUT;
  sw.pull_up_en   = GPIO_PULLUP_ENABLE;
  sw.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&sw);

  gpio_install_isr_service(0);
  gpio_isr_handler_add(PIN_A, encISR, nullptr);
  gpio_isr_handler_add(PIN_B, encISR, nullptr);
}

uint8_t dialpad_buttons() {
  const uint32_t now = tracecraft::hal::millis();

  int16_t d;
  portENTER_CRITICAL(&s_mux);
  d = s_encDelta;
  s_encDelta = 0;
  portEXIT_CRITICAL(&s_mux);
  if (d < 0)      { s_leftUntil  = now + kPulseMs; s_leftBtn  = (uint8_t)(s_page * 2); }
  else if (d > 0) { s_rightUntil = now + kPulseMs; s_rightBtn = (uint8_t)(s_page * 2 + 1); }

  uint8_t b = 0;
  // Signed differences, so they hold across the clock's wrap.
  if ((int32_t)(s_leftUntil  - now) > 0) b |= (uint8_t)(1u << s_leftBtn);
  if ((int32_t)(s_rightUntil - now) > 0) b |= (uint8_t)(1u << s_rightBtn);
  return b;
}
