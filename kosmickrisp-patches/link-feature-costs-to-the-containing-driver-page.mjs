// SPDX-License-Identifier: MIT
// Link feature costs to the containing driver page
export const id = "link-feature-costs-to-the-containing-driver-page";
export function apply({edit, edits}) {
  edit("docs/drivers/kosmickrisp/features.rst", source => source
    .replace("This page lists every capability that KosmicKrisp advertises and how the\ndriver provides it: natively through Metal, by emulation in shaders or compute\npasses, or natively with some cases emulated. Rows that name an entry are\ncontrolled by ``KK_EMULATION``; :doc:`emulation` lists the cost, default and\nreason for each entry. In the performance mode column, ``On`` and ``Off`` give\nthe entry's default in the default ``performance`` mode, ``Always`` marks a\ncapability advertised in every mode, and ``Experimental`` marks one that only\n",
      () => "This page lists every capability that KosmicKrisp advertises and how the\ndriver provides it: natively through Metal, by emulation in shaders or compute\npasses, or natively with some cases emulated. Rows that name an entry are\ncontrolled by ``KK_EMULATION``; the :doc:`main driver page <../kosmickrisp>`\nlists the cost, default and\nreason for each entry. In the performance mode column, ``On`` and ``Off`` give\nthe entry's default in the default ``performance`` mode, ``Always`` marks a\ncapability advertised in every mode, and ``Experimental`` marks one that only\n"));
}
