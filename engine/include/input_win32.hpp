#pragma once

#include <cstdint>
#include <cstdio>

namespace inv {

// Phase 2.7/2.11/2.13/2.14 — live keyboard/mouse → physical axes.
// Prefer DirectInput8; fall back / hybrid with GetAsyncKeyState.
// Logical (virtual) axes come from Input.mapAxis / Controller.user_*.

void input_live_enable(bool on);
bool input_live_enabled();
bool input_di8_ready();        // keyboard device
bool input_di8_mouse_ready();  // mouse device
void input_live_shutdown();

// Last poll's relative mouse deltas (normalized-ish), from DI when ready.
void input_mouse_rel(float* dx, float* dy, float* dz);

// Sample keyboard/mouse into input_set_axis / input_set_last_key.
// Device 0: DIK scan codes (RCDIK_*). Device 1: mouse phys 0..9 (PE).
// Host frame pulse: also runs soft Input_tick — PE order before axis
// consumers; Frontend checkHotkeys adjacency uses this poll.
void input_live_poll();

// Soft PE Input_tick @ 0x0054DDA0 size 0x46 (Engine_MainLoop @ 0x00428AAD).
// frame_dt_sec ≡ Engine_frameDt @ 0x0063C534. Soft globals + deviceListHead
// walk (next @ +0x2144) → Input_Device_tickAxes @ 0x0054DBE0.
// Soft pollDevices @ 0x00556BC0 (kb+mouse; joy/FFB OOS) + soft
// Input_readPhysicalAxis @ 0x00557430 (type 1/2).
// OOS residual: Player+0x1C embed, full DI table stride 490 dwords @ 0x76F9B0,
// Engine_MainLoop wiring (host uses input_live_poll adjacency).
void input_tick(float frame_dt_sec);
float input_frame_dt_ms();       // soft Input_frameDtMs @ 0x007686F4
float input_poll_dt_ms();        // soft Input_pollDtMs @ 0x007686E4
float input_qpc_dt_ms();         // soft Input_qpcDtMs @ 0x0076F9A0
float input_mouse_sens_scale();  // soft Input_mouseSensScale @ 0x00777438
uint32_t input_logical_eval_stamp();  // soft @ 0x007686EC
uint32_t input_device_list_count();   // soft Input_deviceCount @ 0x007686F0
// Soft Input_readPhysicalAxis @ 0x00557430 — DI table type 1/2; joy OOS→0.
float input_read_physical_axis(int32_t device, int32_t axis);
// Soft stand-in for Player+0x1C Push @ 0x00479959 — initAxesBlob@54D580 +
// DeviceList_Push@54D5E0 (one blob if list empty). Called from live_enable.
void input_device_list_ensure();

// PE Input_lastKeyEvent @ 0x00556E00 — DI8 Acquire+GetDeviceData key-down;
// return DIK scan | (ToAsciiEx ascii << 16), or 0.
int32_t input_last_key_event();
// Fork: ASCII for a DirectInput scan code with no modifiers (same ToAsciiEx
// translation as input_last_key_event); 0 when the key has no character.
int32_t input_scan_to_ascii(int32_t scan);

// PE Engine_WndProc @ 0x004B8000 LABEL_34: WM_MOUSEMOVE/LBUTTON{DOWN,UP}/
// RBUTTON{DOWN,UP} → NDC 2*(px/w)-1, 2*(py/h)-1 (Windows Y, top=-1).
void input_wndproc_mouse(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);
bool input_syscursor_ndc(float* x, float* y);
void input_syscursor_set_ndc(float x, float y);
uint32_t input_syscursor_buttons();
void input_syscursor_set_buttons(uint32_t mk);
// PE Engine_SysCursorLock @ 0x004B7C50 / Unlock @ 0x004B7C90.
void input_syscursor_lock();
void input_syscursor_unlock();
bool input_syscursor_locked();
// PE 940 FUN_00427870 / FUN_00425a20: ShowCursor(TRUE / FALSE), one call per
// change of the cursor's enabled bit (`enable`, `disable`, `standby`).
// Nothing in an unattended run (input_mouse_unattended).
void input_syscursor_show(bool show);
// PE 940 FUN_00426cb0: the cursor's `setpos x,y` (window NDC, y down):
// SetCursorPos at that point of the client area. Nothing in an unattended
// run.
void input_syscursor_set_pos(float x, float y);

// ---- Fork: the mouse of unattended runs (script boot only) -----------------
// The exe has none of this. On the legacy path all of it is inert.
//
// True on the script boot when a mouse script (SLRR_PE_BOOT_MOUSE) is given:
// the DirectInput mouse slot is then filled by input_inject_mouse and the
// real device is not read.
bool input_mouse_script();
// True on the script boot when the run is unattended (SLRR_WINDOW_NOACTIVATE=1
// or a mouse script). The real pointer then stays out of the run: window
// mouse messages that do not come from input_inject_mouse are dropped,
// GetCursorPos is not read, and nothing may call ShowCursor, SetCursor,
// SetCursorPos or ClipCursor (state and log only).
bool input_mouse_unattended();

// One step of the mouse script. `op`: 'L' / 'l' left button down / up, 'R' /
// 'r' right button down / up, 'W' wheel by `n` notches (signed; +1 = away
// from the user, lZ +120).
struct InputInjectMouseOp {
  char op = 0;
  int32_t n = 0;
};
struct InputInjectMouseState {
  bool placed = false;        // an entry has put hand and pointer down
  int32_t x = 0, y = 0;       // the pointer, back-buffer pixels
  int32_t hand_x = 0, hand_y = 0;  // the hand (where the next entry counts from)
  int32_t dx = 0, dy = 0;     // DirectInput counts this call added
  bool left = false, right = false;  // held buttons
  bool locked = false;        // the cursor's `lock` held when the call began
  uint32_t polls = 0;         // polls of the device slot so far (the loop
                              // waits for one after a button or wheel entry)
};
// The hand, the OS and the device of an unattended run: moves the hand to
// back-buffer pixel (x, y), then applies `ops` in order. It enters where real
// input enters, the window procedure's mouse messages (input_wndproc_mouse)
// and the DirectInput mouse slot; everything behind those is the real code.
// Does nothing without a mouse script. `out` (optional) = the state after.
void input_inject_mouse(int32_t x, int32_t y, const InputInjectMouseOp* ops,
                        int32_t n_ops, InputInjectMouseState* out);
// The state the next input_inject_mouse call starts from (no input is made).
void input_inject_mouse_peek(InputInjectMouseState* out);
// `[input] mouse axes ...`: what device 1 and the window procedure hold, one
// line at loop end on the script boot (SLRR_PE_NATIVE_PROF). With a mouse
// script a second line, `[input] mouse script ...`: the counts the device
// slot was handed over the run and the largest values published from them.
void input_debug_dump(FILE* out);

// Virtual axis ids (mirror Input.java) — use with user_GetAxisVal / mapAxis.
constexpr int32_t kAxisMoveLR = 4;
constexpr int32_t kAxisMoveUD = 5;
constexpr int32_t kAxisMoveFB = 6;
constexpr int32_t kAxisTurnLR = 1;  // AXIS_TURN_LEFTRIGHT
constexpr int32_t kAxisThrottle = 28;
constexpr int32_t kAxisBrake = 29;
constexpr int32_t kAxisNitro = 31;      // Input.AXIS_NITRO
constexpr int32_t kAxisSelect = 34;
constexpr int32_t kAxisCancel = 35;
constexpr int32_t kAxisCursorX = 42;
constexpr int32_t kAxisCursorY = 43;
constexpr int32_t kAxisCursorZ = 44;
constexpr int32_t kAxisCursorBtn1 = 45;
constexpr int32_t kAxisCursorBtn2 = 46;
constexpr int32_t kAxisCursorBtn3 = 47;
constexpr int32_t kAxisHandbrake = 48;  // Input.AXIS_HANDBRAKE
constexpr int32_t kAxisClutch = 49;     // Input.AXIS_CLUTCH
constexpr int32_t kAxisGearUpDown = 50; // Input.AXIS_GEAR_UPDOWN
constexpr int32_t kAxisMenuUp = 55;
constexpr int32_t kAxisMenuDown = 56;
constexpr int32_t kAxisMenuLeft = 57;
constexpr int32_t kAxisMenuRight = 58;

// Physical mouse axis ids (ControlSet.defaults MOUSE column).
constexpr int32_t kMousePhysX = 0;
constexpr int32_t kMousePhysY = 1;
constexpr int32_t kMousePhysWheel = 2;
constexpr int32_t kMousePhysBtn1 = 3;
constexpr int32_t kMousePhysBtn2 = 4;

// Common DIK (mirror Input.RCDIK_* / dinput.h).
constexpr int32_t kDikReturn = 0x1C;
constexpr int32_t kDikEscape = 0x01;
constexpr int32_t kDikSpace = 0x39;
constexpr int32_t kDikLeft = 0xCB;
constexpr int32_t kDikRight = 0xCD;
constexpr int32_t kDikUp = 0xC8;
constexpr int32_t kDikDown = 0xD0;

}  // namespace inv
