// SPDX-License-Identifier: GPL-2.0

#include <mm/slub.h>
#include <aerosync/export.h>

/*
 * Merge two nullptr-terminated pointer arrays into a newly allocated
 * array, which is also nullptr-terminated. Nomenclature is inspired by
 * memset_p() and memcat() found elsewhere in the kernel source tree.
 */
void **__memcat_p(void **a, void **b)
{
#ifdef CONFIG_STRING_ADVANCED
  void **p = a, **new;
  int nr;

  /* count the elements in both arrays */
  for (nr = 0, p = a; *p; nr++, p++)
    ;
  for (p = b; *p; nr++, p++)
    ;
  /* one for the nullptr-terminator */
  nr++;

  new = kmalloc_array(nr, sizeof(void *), GFP_KERNEL);
  if (!new)
    return nullptr;

  /* nr -> last index; p points to nullptr in b[] */
  for (nr--; nr >= 0; nr--, p = p == b ? &a[nr] : p - 1)
    new[nr] = *p;

  return new;
#endif
}
EXPORT_SYMBOL_GPL(__memcat_p);