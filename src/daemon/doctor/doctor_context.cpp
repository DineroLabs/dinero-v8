// doctor_context.cpp - Read-only environment context
#include "daemon/doctor/doctor_context.h"

namespace dinero {
namespace doctor {

DoctorContext::DoctorContext(const std::string& datadir, const std::string& network)
    : datadir_(datadir), network_(network) {}

} // namespace doctor
} // namespace dinero
