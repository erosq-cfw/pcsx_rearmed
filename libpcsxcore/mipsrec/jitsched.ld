/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 erosqcfw */
/* EROSQ_JITSCHED: append the scheduler's code and read-only data after every
 * existing read-only/executable section, so nothing pre-existing moves
 * (augments the default linker script). */
SECTIONS
{
  .jitsched : { *(.jitsched_text) *(.jitsched_rodata) *(.jitsched_text_tail) *(.jitsched_rodata_tail) }
}
INSERT AFTER .plt;
