// SPDX-License-Identifier: Apache-2.0
// Negative control for the symbol audit: this library uses operator new on purpose, and the
// audit must report it.
int* st_audit_negative_control_make() { return new int(42); }
