// Fragment of DynoData.cpp (m6 step 7): the Build 940 engine cycle behind
// DynoData.calcDyno, transcribed from the decompile (notes/m6/gh/b03..b07,
// exe_mech.md section 6). `w[n]` is word n of the exe's 0x1EC-byte engine
// object (FUN_004f44a0); every value is float32 and every expression keeps
// the exe's order of operations (scalar SSE, no x87). The integer words
// (the step +0x1D0, the count +0x1D4, the pass +0x1D8, the cycle counter
// +0x174) and the table pointers are kept beside the floats.
// Fork: sinf / cosf / powf are the CRT's, not the exe's own 0x570fcb /
// 0x570e7e / 0x571143 (an ulp apart at most [I]). Words the exe leaves
// uninitialised in a fresh object (the fuel and air rates +0x198 / +0x19C,
// the cycle time +0x124 before the first point) start at 0.

struct Dyno940 {
  float w[123] = {};
  int cycles = 0;      // +0x174 (word 0x5D)
  int step = 0;        // +0x1D0 (word 0x74)
  int count = 0;       // +0x1D4 (word 0x75)
  int pass = 0;        // +0x1D8 (word 0x76)
  std::vector<float> torque[2];  // +0x1DC / +0x1E0 (words 0x77 / 0x78)
  std::vector<float> power[2];   // +0x1E4 / +0x1E8 (words 0x79 / 0x7A)
  int cycles_run = 0;  // host: sub-cycles over all points, for the log
};

namespace dyno940 {

constexpr float kOne = 1.0f;                  // 0x593598
constexpr float kAtmPerPa = 9.86923259915784e-06f;  // 0x5A71C4
constexpr float kPi = 3.14159274f;            // 0x5935A4
constexpr float kRpmMax45 = 45.0f;            // 0x5A71D8
constexpr float kRadToRpm = 9.54929638f;      // 0x59AB98
constexpr float kCycle120 = 120.0f;           // 0x59B198
constexpr float kRpmToOmega = 0.104719758f;   // 0x59DD74
constexpr float kDtPerCycle = 0.0043290043f;  // 0x5A71CC
constexpr float kInitDt = 0.005f;             // 0x5A71D0
constexpr float kConverge = 0.05f;            // 0x594798
constexpr float kRelaxOld = 0.4f;             // 0x5947A4
constexpr float kRelaxNew = 0.6f;             // 0x595B20
constexpr float kWToHp = 0.001341f;           // 0x5A71C8
constexpr float kFuelScale = 7920.0f;         // 0x5A71DC
constexpr float kBurnCap = 0.9f;              // 0x59370C
constexpr float kTiny = 1e-08f;               // 0x59B184
constexpr float kPerMinute = 0.0166666675f;   // 0x59DD68
constexpr float kTorqueK = 0.125f;            // 0x595B18
constexpr float kFriction = 2e-08f;           // 0x5A71C0
constexpr float kFourPi = 12.566371f;         // 0x5A71D4
constexpr float kMinV = 1e-07f;               // 0x5938C0
constexpr float kFlow = 1000.0f;              // 0x59746C
constexpr float kMinP = 1e-06f;               // 0x59A450
constexpr float kExpP = 1.3f;                 // 0x59AB6C
constexpr float kExpT = 0.3f;                 // 0x593704
constexpr float kHalf = 0.5f;                 // 0x593590
constexpr float kNitroEps = 1e-04f;           // 0x5938C4

float bits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// FUN_004f4fc0: the state of a cycle at crank angle 0.
void reset(Dyno940& o) {
  float* p = o.w;
  const float r = p[0];
  const float L = p[3];
  p[0x52] = p[0x55];
  p[0x50] = p[0x56];
  p[0x48] = 0.0f;
  p[0x4b] = 0.0f;
  p[0x40] = 0.0f;
  p[0x41] = 0.0f;
  p[0x42] = 0.0f;
  p[0x46] = 0.0f;
  p[0x47] = 0.0f;
  p[0x5e] = 0.0f;
  p[0x5f] = 0.0f;
  p[0x61] = 0.0f;
  p[100] = 0.0f;
  p[0x51] = p[0x57];
  p[0x5a] = 0.0f;
  p[0x5b] = 0.0f;
  const float x = std::sqrt(L * L - r * 0.0f * r * 0.0f) + r;
  p[0x4e] = p[0x58];
  p[0x4c] = x;
  const float v = ((x - L) - r) * p[0x1a] + p[4];
  p[0x4d] = v;
  p[0x4f] = v * p[0x58];
}

// FUN_004f49e0: every word to its default.
void defaults(Dyno940& o) {
  float* p = o.w;
  p[9] = bits(0x3fa57dbf);
  p[0xb] = bits(0x47c5e680);
  p[0xc] = bits(0x47d9b0c0);
  p[0x3d] = bits(0x41600000);
  p[0] = bits(0x3d59ce07);
  p[1] = bits(0x3d48b439);
  p[3] = bits(0x3e19999a);
  p[4] = bits(0x38d1b717);
  p[5] = bits(0x40800000);
  p[7] = bits(0x3b449ba6);
  p[8] = bits(0x3851b717);
  p[2] = bits(0x3f83b646);
  p[6] = bits(0x3f800000);
  p[0xf] = bits(0x49742400);
  p[0x10] = 0.0f;
  p[10] = bits(0x44320000);
  p[0xd] = bits(0x43929333);
  p[0xe] = bits(0x43929333);
  p[0x11] = bits(0x3ba3d70a);
  p[0x12] = bits(0x3ba3d70a);
  p[0x13] = bits(0x3f4ccccd);
  p[0x14] = bits(0x3f4ccccd);
  p[0x15] = bits(0x42be0000);
  p[0x44] = bits(0x43b09333);
  p[0x45] = bits(0x43b09333);
  p[0x24] = bits(0x3ef5c28f);
  p[0x25] = bits(0x3a83126f);
  p[0x26] = bits(0x3e800000);
  p[0x27] = bits(0x3f400000);
  p[0x28] = bits(0x3f7d70a4);
  p[0x33] = bits(0x3ef5c28f);
  p[0x34] = bits(0xbd4ccccd);
  p[0x35] = bits(0x44fa0000);
  p[0x36] = bits(0x459c4000);
  p[0x29] = bits(0x3a83126f);
  p[0x2a] = bits(0x3e7ef9db);
  p[0x2b] = 0.0f;
  p[0x2c] = 0.0f;
  p[0x2d] = 0.0f;
  p[0x2e] = bits(0x3f400000);
  p[0x2f] = bits(0x3e75c290);
  p[0x30] = 0.0f;
  p[0x31] = 0.0f;
  p[0x32] = 0.0f;
  p[0x48] = 0.0f;
  p[0x4b] = 0.0f;
  p[0x22] = bits(0x447a0000);
  p[0x37] = bits(0x41a00000);
  p[0x38] = bits(0x47a60400);
  p[0x3a] = bits(0x46ea6000);
  const float f = kOne / (p[0x3d] + kOne);
  p[0x39] = 0.0f;
  p[0x3b] = 0.0f;
  p[0x3c] = bits(0x40400000);
  p[0x54] = f;
  p[0x53] = f * p[0x3d];
  p[0x1f] = p[0xb] * kAtmPerPa * p[9];
  p[0x20] = p[0xc] * kAtmPerPa * p[9];
  reset(o);
  const float dt = p[0x49] * kInitDt;
  p[0x6d] = 0.0f;
  p[0x6e] = 0.0f;
  p[0x6a] = 0.0f;
  p[0x21] = dt;
  p[0x72] = 0.0f;
  p[0x73] = 0.0f;
  p[0x6f] = 0.0f;
  p[0x3f] = bits(0x42c80000);
  p[0x3e] = bits(0x42c80000);
  p[0x5a] = 0.0f;
  p[0x5b] = 0.0f;
}

// FUN_004f5980: the mixture and the charge densities.
void mixture(Dyno940& o) {
  float* p = o.w;
  const float f = kOne / (p[0x3d] + kOne);
  p[0x54] = f;
  p[0x53] = f * p[0x3d];
  p[0x1f] = p[0xb] * kAtmPerPa * p[9];
  p[0x20] = p[0xc] * kAtmPerPa * p[9];
}

// FUN_004f59f0: the geometry.
void geometry(Dyno940& o) {
  float* p = o.w;
  float rb = p[1];
  const float rc = p[0];
  p[0x55] = p[0xf];
  p[0x56] = p[0xb];
  const float area = rb * rb * kPi;
  p[0x17] = rb + rb;
  p[0x18] = rc + rc;
  rb = (rc + rc) * area;
  p[0x57] = p[0xd];
  p[0x1a] = area;
  p[0x1b] = rb;
  p[0x1e] = 45.0f;
  p[0x58] = p[0x1f];
  p[0x1c] = rb + p[4];
  p[0x1d] = (rb + p[4]) / p[4];
  p[0x23] = (kRpmMax45 / rc) * kRadToRpm;
  p[0x5c] = p[0x4a] * rc;
  p[0x19] = p[5] * rb;
}

// FUN_004f4ca0: valve lifts, spark advance and turbo boost for the RPM.
void setup(Dyno940& o) {
  float* p = o.w;
  const float one = kOne;
  float f4 = kOne / (p[0x3d] + kOne);
  const float f5 = p[0xb] * kAtmPerPa * p[9];
  p[0x54] = f4;
  p[0x1f] = f5;
  p[0x53] = f4 * p[0x3d];
  p[0x20] = p[0xc] * kAtmPerPa * p[9];
  f4 = p[0x36] - p[0x35];
  if ((f4 <= 0.0f) || (f4 = (p[0x22] - p[0x35]) / f4, f4 < 0.0f)) {
    p[0x24] = p[0x33];
  } else {
    float f2 = p[0x34];
    if (f4 <= one) f2 = f2 * f4;
    p[0x24] = f2 + p[0x33];
  }
  f4 = p[0x2d] - p[0x2c];
  if ((f4 <= 0.0f) || (f4 = (p[0x22] - p[0x2c]) / f4, f4 < 0.0f)) {
    p[0x25] = p[0x29];
  } else {
    float f2 = p[0x2b];
    if (f4 <= one) f2 = f2 * f4;
    p[0x25] = f2 + p[0x29];
  }
  p[0x26] = p[0x2a] + p[0x25];
  f4 = p[0x32] - p[0x31];
  if ((f4 <= 0.0f) || (f4 = (p[0x22] - p[0x31]) / f4, f4 < 0.0f)) {
    p[0x27] = p[0x2e];
  } else {
    float f2 = p[0x30];
    if (f4 <= one) f2 = f2 * f4;
    p[0x27] = f2 + p[0x2e];
  }
  p[0x56] = p[0xb];
  p[0x28] = p[0x2f] + p[0x27];
  p[0x55] = p[0xf];
  p[0x57] = p[0xd];
  p[0x58] = f5;
  if ((0.0f < p[0x37]) && (0.0f < p[0x3a])) {
    const float lo = p[0x39];
    const float d = std::fabs(p[0x22] * p[0x37] - p[0x38]);
    float f2 = one;
    if ((lo <= d) && (f2 = one - (d - lo) / (p[0x3a] - lo), f2 <= 0.0f)) f2 = 0.0f;
    f2 = p[0x3b] * f2;
    if (p[0x3c] <= f2) f2 = p[0x3c];
    const float s = std::sqrt(f2 + one);
    p[0x57] = p[0xd] * s;
    p[0x58] = f5 * s;
    p[0x56] = s * s * p[0xb];
  }
  f4 = p[0x3e];
  if ((0.0f < f4) && (f4 < p[0x66])) p[0x55] = p[0xf] / (p[0x66] / f4);
  f4 = p[0x3f];
  if ((0.0f < f4) && (f4 < p[0x67])) p[0x56] = p[0x56] / (p[0x67] / f4);
  f4 = p[0x22] * p[8];
  if (kBurnCap <= f4) f4 = kBurnCap;
  p[0x59] = (one - f4) * p[7];
}

// FUN_004f50e0(n): n crank sub-steps.
void substep(Dyno940& o, int n) {
  float* p = o.w;
  if (n <= 0) return;
  const float omega = p[0x4a];
  const float dt = p[0x21];
  const float L = p[3];
  const float tc = p[0x49];
  float vprev = p[0x4d];
  float t = p[0x48];
  float ang = p[0x4b];
  const float r = p[0];
  float vmin = p[4];
  float vmax = p[0x1c];
  float temp = p[0x51];
  float burn = p[0x42];
  float pres = p[0x50];
  float xpos = p[0x4c];
  do {
    const float burn0 = burn;
    const float pres0 = pres;
    if (vmin < 0.0f) vmin = 0.0f;
    if (vmax < 0.0f) vmax = 0.0f;
    float v0 = vprev;
    if (vprev < 0.0f) v0 = 0.0f;
    t = t + p[0x21];
    if (tc <= t) {
      o.cycles = o.cycles + 1;
      t = t - tc;
      const float swept = vmax - vmin;
      p[0x5f] = p[0x5e];
      p[0x5e] = 0.0f;
      if (swept <= kTiny) p[0x61] = 0.0f;
      else p[0x61] = p[0x5f] / swept;
      const float rpm = p[0x22];
      float pw = rpm * p[0x61] * swept * kPerMinute;
      p[0x62] = p[5] * p[0x61] * swept * kTorqueK;
      pw = pw * p[5];
      p[100] = pw;
      const float fr = rpm * rpm * rpm * kFriction;
      pw = pw - fr;
      p[0x60] = fr;
      p[0x65] = pw;
      p[99] = pw * kWToHp;
    }
    ang = ang + omega * dt;
    if (kFourPi < ang) ang = ang - kFourPi;
    const float sn = std::sin(ang);
    const float cs = std::cos(ang);
    // asm 0x4f52e3..0x4f52f7: (sin x r) squared, from L^2; then + cos x r
    const float sr = sn * p[0];
    xpos = std::sqrt(L * L - sr * sr) + cs * p[0];
    const float v = ((r + L) - xpos) * p[0x1a] + vmin;
    vprev = kMinV;
    if (kMinV <= v) {
      const float inv = kOne / v;
      const float ratio = inv * v0;
      pres = std::pow(ratio, kExpP) * pres;
      temp = std::pow(ratio, kExpT) * temp;
      // the intake valve
      float lift = p[0x25] * tc;
      if (p[0x26] <= p[0x25]) {
        if (t <= lift) lift = (t < p[0x26] * tc) ? p[0x13] : p[0x11];
        else lift = p[0x13];
      } else {
        if (lift < t) lift = (t < p[0x26] * tc) ? p[0x13] : p[0x11];
        else lift = p[0x11];
      }
      p[0x40] = lift;
      // the exhaust valve
      lift = p[0x27] * tc;
      if (p[0x28] <= p[0x27]) {
        if (t <= lift) lift = (t < p[0x28] * tc) ? p[0x14] : p[0x12];
        else lift = p[0x14];
      } else {
        if (lift < t) lift = (t < p[0x28] * tc) ? p[0x14] : p[0x12];
        else lift = p[0x12];
      }
      const float spark = p[0x24] * tc;
      p[0x41] = lift;
      float bend = p[0x59] + spark;
      p[0x43] = spark;
      if (tc < bend) bend = bend - tc;
      // the burn: 0 = none, 1 = the heat release block (FUN_004f50e0's
      // LAB_004f5628)
      int heat = 0;
      if (bend <= spark) {
        if ((spark < t) || (t <= bend)) {
          burn = (t - spark) / p[0x59];
          if (0.0f < burn) heat = 1;
        } else if (burn <= 0.0f) {
          burn = 0.0f;
        } else {
          burn = kOne;
          heat = 1;
        }
      } else if ((t <= spark) || (bend < t)) {
        if (0.0f < burn) {
          burn = kOne;
          heat = 1;
        } else {
          burn = 0.0f;
        }
      } else {
        burn = (t - spark) / p[0x59];
        if (0.0f < burn) heat = 1;
      }
      if (heat) {
        const float m = p[0x4f];
        if (m < kMinV) p[0x4f] = 1e-07f;
        float t0 = temp;
        if (temp < kMinV) t0 = kMinV;
        temp = ((burn - burn0) * p[0x52] * m) / (p[10] * p[0x4f]) + t0;
        pres = (temp / t0) * pres;
        if (kOne <= burn) {
          burn = 0.0f;
          p[0x52] = 0.0f;
        }
      }
      if (0.0f < p[0x40]) {
        float k = p[0x40] * p[0x21] * kFlow;
        float pv = v * pres;
        if (kOne <= k) k = kOne;
        pres = k * (p[0x56] - pres) + pres;
        if (pres <= kMinP) pres = kMinP;
        pv = pv / pres;
        if (v <= pv) {
          p[0x4f] = p[0x4f] - (pv - v) * inv * p[0x4f];
        } else {
          const float dm = (pres / p[0x56]) * p[0x58] * (v - pv);
          if (p[0x4f] < kMinV) p[0x4f] = 1e-07f;
          const float m = p[0x4f];
          const float im = kOne / (m + dm);
          p[0x4f] = m + dm;
          temp = (p[0x57] * dm + temp * m) * im;
          p[0x5b] = dm * p[0x53] + p[0x5b];
          p[0x52] = (p[0x55] * dm + p[0x52] * m) * im;
          p[0x5a] = p[0x54] * dm + p[0x5a];
        }
        if (p[0x4f] < 0.0f) p[0x4f] = 0.0f;
      }
      if (0.0f < p[0x41]) {
        float k = p[0x41] * p[0x21] * kFlow;
        float pv = v * pres;
        if (kOne <= k) k = kOne;
        pres = k * (p[0xc] - pres) + pres;
        pv = pv / pres;
        if (v <= pv) {
          p[0x4f] = p[0x4f] - (pv - v) * inv * p[0x4f];
        } else {
          const float dm = (pres / p[0xc]) * p[0x20] * (v - pv);
          if (p[0x4f] < kMinV) p[0x4f] = 1e-07f;
          const float m = p[0x4f];
          const float im = kOne / (m + dm);
          p[0x4f] = m + dm;
          temp = (temp * m + dm * p[0xe]) * im;
          p[0x52] = (p[0x10] * dm + p[0x52] * m) * im;
        }
      }
      if (p[0x4f] < 0.0f) p[0x4f] = 0.0f;
      // the heat loss
      const float dT = temp - p[0x44];
      float k = p[0x21] / p[0x15];
      if (kOne <= p[0x21] / p[0x15]) k = kOne;
      float sgn;
      if (dT == 0.0f) sgn = 0.0f;
      else sgn = (0.0f <= dT) ? kOne : -1.0f;
      float loss = dT * dT * sgn * k;
      if (kOne < loss / dT) loss = loss / (loss / dT);
      const float tnew = temp - loss;
      p[0x46] = p[0x46] + loss;
      if (temp < kMinV) temp = kMinV;
      pres = (tnew / temp) * pres;
      p[0x5e] = (pres + pres0) * kHalf * (v - v0) + p[0x5e];
      vprev = v;
      temp = tnew;
    }
  } while (--n != 0);
  p[4] = vmin;
  p[0x1c] = vmax;
  p[0x42] = burn;
  p[0x4d] = vprev;
  p[0x50] = pres;
  p[0x51] = temp;
  p[0x48] = t;
  p[0x4b] = ang;
  p[0x4c] = xpos;
}

// FUN_004f4560: one full cycle and its per-second consumption.
void cycle(Dyno940& o) {
  float* p = o.w;
  p[0x5a] = 0.0f;
  p[0x5b] = 0.0f;
  p[0x46] = 0.0f;
  p[0x47] = 0.0f;
  o.cycles = 0;
  int guard = 0;
  do {
    substep(o, 1);
  } while (o.cycles == 0 && ++guard < 1000000);
  const float f = kOne / p[0x49];
  const float fuel = p[0x5a] * f * p[5];
  p[0x67] = p[0x5b] * f * p[5];
  p[0x66] = fuel;
  p[0x68] = (fuel * kFuelScale) / p[0x63];
  p[0x69] = (p[0x5b] + p[0x5a]) / (p[0x1b] * p[9]);
  p[0x46] = f * p[0x46];
  ++o.cycles_run;
}

// FUN_004f4640(step): the torque and power tables of pass o.pass.
void tables(Dyno940& o, int step) {
  float* p = o.w;
  p[0x6d] = 0.0f;
  p[0x6e] = 0.0f;
  p[0x6a] = 0.0f;
  p[0x6b] = 0.0f;
  p[0x6c] = 0.0f;
  p[0x72] = 0.0f;
  p[0x73] = 0.0f;
  p[0x6f] = 0.0f;
  p[0x70] = 0.0f;
  if (step == 0) step = 1;  // Fork: the exe divides by zero
  o.count = static_cast<int>(p[0x23]) / step;
  p[0x71] = 0.0f;
  o.step = step;
  const int k = o.pass;
  const int n = o.count > 0 ? o.count : 0;
  o.torque[k].assign(static_cast<size_t>(n), 0.0f);
  o.power[k].assign(static_cast<size_t>(n), 0.0f);
  int rpm_i = step;
  for (int i = 1; i < o.count; ++i) {
    const float rpm = static_cast<float>(rpm_i);
    p[0x4b] = 0.0f;
    p[0x22] = rpm;
    const float om = rpm * kRpmToOmega;
    p[0x49] = kCycle120 / rpm;
    p[0x4a] = om;
    p[0x48] = (kCycle120 / rpm) * 0.0f;
    p[0x5c] = om * p[0];
    setup(o);
    p[0x21] = p[0x49] * kDtPerCycle;
    reset(o);
    cycle(o);
    float last = p[0x61];
    int left = 10;
    do {
      cycle(o);
      if (std::fabs(last / p[0x61] - kOne) < kConverge) break;
      --left;
      last = last * kRelaxOld + p[0x61] * kRelaxNew;
    } while (0 < left);
    float x = p[100];
    if (p[0x6d] <= x && x != p[0x6d]) {
      p[0x6d] = x;
      p[0x72] = p[0x22];
    }
    x = p[0x65];
    if (p[0x6e] <= x && x != p[0x6e]) {
      p[0x6e] = x;
      x = x * kWToHp;
      p[0x73] = p[0x22];
      p[0x71] = p[0x22];
      p[0x6c] = x;
    }
    x = p[0x61];
    if (p[0x6a] <= x && x != p[0x6a]) {
      p[0x6a] = x;
      p[0x6f] = p[0x22];
      p[0x70] = p[0x22];
      p[0x6b] = p[0x62];
    }
    o.torque[k][static_cast<size_t>(i)] = p[0x62];
    o.power[k][static_cast<size_t>(i)] = p[0x65];
    rpm_i = rpm_i + step;
  }
  p[0x62] = p[0x6b];
  p[100] = p[0x6d];
  p[0x65] = p[0x6e];
}

}  // namespace dyno940

std::unordered_map<InvObject*, Dyno940>& g_dyno940 =
    *new std::unordered_map<InvObject*, Dyno940>();

// PE 940 DynoData.calcDyno 0x4CBA80 (FUN_004cba80) for a DynoData with its
// native object (the class's constructor calls newNative): the temporary
// object's branch (table_stepsize, torquetable, HPtable, torque2) is not
// reached by any shipped class and is not ported.
float dyno940_calc(InvObject* self, float tablesize) {
  using namespace dyno940;
  Dyno940& o = g_dyno940[self];
  float* p = o.w;
  o.cycles_run = 0;
  defaults(o);
  auto f = [&](const char* n) { return tree_field_get_float(self, n); };
  p[5] = f("cylinders");
  p[1] = f("bore") * kHalf;
  p[0] = f("stroke") * kHalf;
  p[4] = f("Vmin");
  p[0x11] = f("in_min");
  p[0x13] = f("in_max");
  p[0x12] = f("out_min");
  p[0x14] = f("out_max");
  p[0x25] = f("time_in_open");
  p[0x26] = f("time_in_close");
  p[0x27] = f("time_out_open");
  const float out_close = f("time_out_close");
  p[0x29] = p[0x25];
  p[0x28] = out_close;
  p[0x2a] = p[0x26] - p[0x25];
  p[0x2e] = p[0x27];
  p[0x2f] = out_close - p[0x27];
  p[7] = f("time_burn");
  p[0x16] = f("RPM_limit");
  p[0x33] = f("time_spark_min");
  p[0x34] = f("time_spark_inc");
  p[0x35] = f("time_spark_RPM0");
  p[0x36] = f("time_spark_RPM1");
  p[0x37] = f("rpm_turbo_mul");
  p[0x38] = f("rpm_turbo_opt");
  p[0x3a] = f("rpm_turbo_range");
  p[0x39] = 0.0f;
  p[0x3b] = f("P_turbo_max");
  // FUN_004ecff0 finds both optional fields: DynoData.java declares them.
  p[0x3c] = f("P_turbo_waste");
  p[0xf] = f("mixture_H");
  p[0x3d] = f("mixture_ratio");
  p[0x3f] = f("max_air_consumption");
  p[0x3e] = f("max_fuel_consumption");
  p[0xd] = 293.15f;
  p[0xd] = p[0xd] * f("intercooling");
  mixture(o);
  geometry(o);
  const float max_rpm = f("maxRPM");
  if (0.0f < max_rpm) p[0x23] = max_rpm;
  else tree_field_set_float(self, "maxRPM", p[0x23]);
  o.pass = 0;
  float st = p[0x23] / tablesize;
  if (p[0x23] / tablesize <= kOne) st = kOne;
  tables(o, static_cast<int>(st));
  tree_field_set_float(self, "Displacement", p[0x19]);
  tree_field_set_float(self, "Compression", p[0x1d]);
  tree_field_set_float(self, "maxTorque", p[0x6b]);
  tree_field_set_float(self, "RPM_maxTorque", p[0x70]);
  tree_field_set_float(self, "maxHP", p[0x6c]);
  tree_field_set_float(self, "RPM_maxHP", p[0x71]);
  const int first_step = o.step, first_count = o.count;
  p[0xd] = 293.15f;
  const float nh = f("nitro_H");
  const float nc = f("nitro_cooling");
  bool nitro = false;
  if ((kNitroEps < nh - kOne) || (kNitroEps < nc - kOne)) {
    nitro = true;
    p[0xf] = p[0xf] * nh;
    p[0xd] = p[0xd] * nc;
    const float keep = p[0x23];
    mixture(o);
    geometry(o);
    p[0x23] = keep;
    o.pass = 1;
    tables(o, static_cast<int>(keep / tablesize));
  }
  if (native_type_log()) {
    std::fprintf(stderr,
                 "[dyno] out Displacement=%.6g Compression=%.6g maxTorque=%.6g RPM_maxTorque=%.6g "
                 "maxHP=%.6g RPM_maxHP=%.6g points=%d step=%d cycles=%d nitro=%d\n",
                 p[0x19], p[0x1d], f("maxTorque"), f("RPM_maxTorque"), f("maxHP"),
                 f("RPM_maxHP"), first_count, first_step, o.cycles_run, nitro ? 1 : 0);
  }
  // getTorque / getHP read the first pair (+0x1DC, +0x1E4) with the step and
  // the count of the LAST table built (+0x1D0 / +0x1D4), as the exe does.
  return 0.0f;
}

// PE 940 getTorque 0x4CC2B0 / getHP 0x4CC1B0: linear interpolation in the
// first pair of tables with the object's step and count; 0 below 0 RPM,
// beyond the last point, and without a native object.
bool dyno940_sample(InvObject* self, bool power, float rpm, float* out) {
  auto it = g_dyno940.find(self);
  if (it == g_dyno940.end()) return false;
  const Dyno940& o = it->second;
  *out = 0.0f;
  if (!(0.0f <= rpm)) return true;
  const std::vector<float>& t = power ? o.power[0] : o.torque[0];
  int i = 1;
  if (1 < o.count && o.step > 0 && static_cast<int>(t.size()) >= o.count) {
    const int s = o.step;
    int acc = s;
    while (static_cast<float>(acc) <= rpm) {
      ++i;
      acc += s;
      if (o.count <= i) return true;
    }
    *out = ((rpm - static_cast<float>((i - 1) * s)) * (t[static_cast<size_t>(i)] -
                                                       t[static_cast<size_t>(i - 1)])) /
               static_cast<float>(s) +
           t[static_cast<size_t>(i - 1)];
  }
  return true;
}
