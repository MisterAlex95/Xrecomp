# DVD SCSI pass-through

Regression using synthetic guest memory and a synthetic certificate only; no
game assets. Sends `IOCTL_SCSI_PASS_THROUGH_DIRECT` to the synthetic DVD
handle and checks the status is GOOD and:

- READ DVD STRUCTURE (0xAD), 64 bytes, no certificate: the DataBuffer is
  zeroed;
- 0xAD, 1636 bytes, a certificate with bit 0 set at +0x1D8: the security
  sector for it (the certificate's time stamp at +0x4AB, the SHA-1 at +0x4CF
  checked against a value computed with hashlib, zeros elsewhere, nothing
  written past the buffer), docs/runtime/README.md « DVD security sector »;
- the same certificate with a 64-byte buffer, another command (INQUIRY), the
  bit clear, or a certificate too short to have the field: zeroed.

```powershell
cmake -S tests/kernel_dvd_scsi -B build/dvd-scsi -A x64
cmake --build build/dvd-scsi --config Release --target kernel_dvd_scsi_test
ctest --test-dir build/dvd-scsi -C Release --output-on-failure
```

POSIX: `cmake -S tests/kernel_dvd_scsi -B build/dvd-scsi -G Ninja && cmake --build build/dvd-scsi && build/dvd-scsi/kernel_dvd_scsi_test`.
