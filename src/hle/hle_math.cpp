// libm (soft-float calling convention: values travel in core registers).
#include <cmath>

#include "hle.h"

namespace zb::hle {
namespace {

#define D1(name) \
    double m_##name(double x) { return std::name(x); }
#define F1(name) \
    float m_##name##f(float x) { return std::name(x); }

D1(acos) D1(asin) D1(atan) D1(ceil) D1(cos) D1(cosh) D1(exp) D1(exp2) D1(floor) D1(log) D1(log10)
D1(sin) D1(sinh) D1(sqrt) D1(tan) D1(tanh) D1(trunc)
F1(acos) F1(asin) F1(atan) F1(ceil) F1(cos) F1(floor) F1(log10) F1(sin) F1(sqrt)

double m_atan2(double y, double x) { return std::atan2(y, x); }
float m_atan2f(float y, float x) { return std::atan2(y, x); }
double m_fmod(double x, double y) { return std::fmod(x, y); }
double m_pow(double x, double y) { return std::pow(x, y); }
float m_powf(float x, float y) { return std::pow(x, y); }
double m_ldexp(double x, i32 e) { return std::ldexp(x, e); }
double m_frexp(double x, gp<i32> e) { return std::frexp(x, e.get()); }
double m_modf(double x, gp<u8> ip) {
    double i;
    double f = std::modf(x, &i);
    memcpy(ip.get(), &i, 8);
    return f;
}

}  // namespace

void register_math() {
    REG(acos, m_acos);
    REG(asin, m_asin);
    REG(atan, m_atan);
    REG(ceil, m_ceil);
    REG(cos, m_cos);
    REG(cosh, m_cosh);
    REG(exp, m_exp);
    REG(exp2, m_exp2);
    REG(floor, m_floor);
    REG(log, m_log);
    REG(log10, m_log10);
    REG(sin, m_sin);
    REG(sinh, m_sinh);
    REG(sqrt, m_sqrt);
    REG(tan, m_tan);
    REG(tanh, m_tanh);
    REG(trunc, m_trunc);
    REG(acosf, m_acosf);
    REG(asinf, m_asinf);
    REG(atanf, m_atanf);
    REG(ceilf, m_ceilf);
    REG(cosf, m_cosf);
    REG(floorf, m_floorf);
    REG(log10f, m_log10f);
    REG(sinf, m_sinf);
    REG(sqrtf, m_sqrtf);
    REG(atan2, m_atan2);
    REG(atan2f, m_atan2f);
    REG(fmod, m_fmod);
    REG(pow, m_pow);
    REG(powf, m_powf);
    REG(ldexp, m_ldexp);
    REG(frexp, m_frexp);
    REG(modf, m_modf);
}

}  // namespace zb::hle
