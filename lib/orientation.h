#pragma once
#include <cmath>
#include <cstdint>
#include <string>

namespace Mist {
  // RF: clockwise rotation followed by a left-right mirror. EXIF 1..8 maps to
  // (0,F), (0,T), (180,F), (180,T), (90,T), (90,F), (270,T), (270,F).
  struct Orientation {
      uint16_t rotation;
      bool mirror;
      Orientation(uint16_t angle = 0, bool reflected = false)
        : rotation(angle < 360 && angle % 90 == 0 ? angle : 0), mirror(reflected) {}
      bool operator==(const Orientation & other) const { return rotation == other.rotation && mirror == other.mirror; }
      bool operator!=(const Orientation & other) const { return !(*this == other); }
      void matrix(double & a, double & b, double & c, double & d) const {
        const int cosine = rotation == 0 ? 1 : rotation == 180 ? -1 : 0;
        const int sine = rotation == 90 ? 1 : rotation == 270 ? -1 : 0;
        a = mirror ? -cosine : cosine;
        b = sine;
        c = mirror ? sine : -sine;
        d = cosine;
      }
      static bool fromMatrix(double a, double b, double c, double d, Orientation & out) {
        const double x = std::hypot(a, b), y = std::hypot(c, d);
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0.000001 || y < 0.000001) { return false; }
        a /= x;
        b /= x;
        c /= y;
        d /= y;
        for (unsigned turn = 0; turn < 4; ++turn) {
          for (bool flip : {false, true}) {
            Orientation candidate(turn * 90, flip);
            double ca, cb, cc, cd;
            candidate.matrix(ca, cb, cc, cd);
            if (std::abs(a - ca) < 0.001 && std::abs(b - cb) < 0.001 && std::abs(c - cc) < 0.001 && std::abs(d - cd) < 0.001) {
              out = candidate;
              return true;
            }
          }
        }
        return false;
      }
      // ISO BMFF uses row vectors: apply this track matrix, then the movie matrix.
      Orientation then(const Orientation & movie) const {
        return Orientation((rotation + (mirror ? 360 - movie.rotation : movie.rotation)) % 360, mirror != movie.mirror);
      }
      double projectionRoll() const {
        const int degrees = mirror ? rotation : -int(rotation);
        return degrees <= -180 ? degrees + 360 : degrees > 180 ? degrees - 360 : degrees;
      }
      static bool fromProjection(double yaw, double pitch, double roll, Orientation & out) {
        if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll) || std::abs(pitch) > 0.001 ||
            (std::abs(yaw) > 0.001 && std::abs(std::abs(yaw) - 180) > 0.001) || std::abs(roll) > 180) {
          return false;
        }
        const bool flip = std::abs(yaw) > 0.001;
        const double angle = flip ? roll : -roll;
        const int turn = std::lround(angle / 90.0);
        if (std::abs(angle - turn * 90) > 0.001) { return false; }
        out = Orientation((turn % 4 + 4) % 4 * 90, flip);
        return true;
      }
      // Hardware transpose supports reversal/flip directions; CPU transpose does not.
      std::string filters(const std::string & backend = "cpu") const {
        const char *direction = rotation == 90 ? (mirror ? "cclock_flip" : "clock")
          : rotation == 270                    ? (mirror ? "clock_flip" : "cclock")
          : rotation == 180                    ? (mirror ? "vflip" : "reversal")
          : mirror                             ? "hflip"
                                               : "";
        if (!*direction) { return ""; }
        if (backend != "cpu") { return "transpose_" + backend + "=dir=" + direction; }
        if (!rotation) { return "hflip"; }
        if (rotation == 180) { return mirror ? "vflip" : "hflip,vflip"; }
        return std::string("transpose=") + direction;
      }
  };
} // namespace Mist
