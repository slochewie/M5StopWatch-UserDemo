#!/usr/bin/env bash
set -euo pipefail

H="components/M5PM1/src/M5PM1.h"
CPP="components/M5PM1/src/M5PM1.cpp"
HAL="main/hal/hal_pmic.cpp"

for f in "$H" "$CPP" "$HAL"; do
  if [[ ! -f "$f" ]]; then
    echo "ERROR: $f not found. Run from repo root."
    exit 1
  fi
done

cp "$H" "$H.raw-register-probe.bak"
cp "$CPP" "$CPP.raw-register-probe.bak"
cp "$HAL" "$HAL.raw-register-probe.bak"

python3 - <<'PY'
from pathlib import Path

h = Path("components/M5PM1/src/M5PM1.h")
cpp = Path("components/M5PM1/src/M5PM1.cpp")
hal = Path("main/hal/hal_pmic.cpp")

hs = h.read_text()
if "readRegisterRaw" not in hs:
    anchor = "    m5pm1_err_t clearWakeSource(uint8_t mask);\n"
    if anchor not in hs:
        raise SystemExit("ERROR: Could not find clearWakeSource declaration in M5PM1.h")
    hs = hs.replace(anchor, anchor + "    m5pm1_err_t readRegisterRaw(uint8_t reg, uint8_t* value);\n", 1)
    h.write_text(hs)

cs = cpp.read_text()
if "M5PM1::readRegisterRaw" not in cs:
    anchor = "m5pm1_err_t M5PM1::clearWakeSource(uint8_t mask)"
    if anchor not in cs:
        raise SystemExit("ERROR: Could not find clearWakeSource implementation in M5PM1.cpp")
    method = """m5pm1_err_t M5PM1::readRegisterRaw(uint8_t reg, uint8_t* value)
{
    if (value == nullptr) {
        return M5PM1_ERR_INVALID_PARAM;
    }

    return _readReg(reg, value) ? M5PM1_OK : M5PM1_ERR_I2C_COMM;
}

"""
    cs = cs.replace(anchor, method + anchor, 1)
    cpp.write_text(cs)

s = hal.read_text()
s = s.replace("->readRegister(", "->readRegisterRaw(")
hal.write_text(s)
PY

echo "Fixed PMG0 wake register probe raw-register access."
echo
echo "Now build:"
echo "  idf.py build"
echo
echo "Then flash/monitor:"
echo "  idf.py flash monitor"
