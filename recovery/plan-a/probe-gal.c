/* SPDX-License-Identifier: MIT
 * Plan A GPU handshake probe. Linked against the stock libGAL.so (Vivante
 * 5.0.11.p5.28018) with a glibc 2.27 toolchain, run on the 4.19 RAM kernel
 * after loading the SDK galcore.ko. Read-only GPU queries; no rendering.
 *
 * Exit 0 only if the user library constructs, the kernel reports the same
 * HAL version the headers were built for, and chip identity is readable.
 */
#include <stdio.h>
#include <string.h>
#include "HAL/gc_hal.h"
#include "HAL/gc_hal_driver.h"

#define CHECK(what, status) do { \
	gceSTATUS s_ = (status); \
	printf("%-28s status=%d\n", what, (int)s_); \
	if (gcmIS_ERROR(s_)) return 1; \
} while (0)

int main(void)
{
	gcoOS os = gcvNULL;
	gcoHAL hal = gcvNULL;
	gcsHAL_INTERFACE iface;
	gceCHIPMODEL model;
	gctUINT32 revision, features, minor;
	gctPHYS_ADDR ia, ea, ca;
	gctSIZE_T is, es, cs;

	printf("probe-gal: headers %s (%d.%d.%d build %d)\n", gcvVERSION_STRING,
	       gcvVERSION_MAJOR, gcvVERSION_MINOR, gcvVERSION_PATCH, gcvVERSION_BUILD);
	CHECK("gcoOS_Construct", gcoOS_Construct(gcvNULL, &os));
	CHECK("gcoHAL_ConstructEx", gcoHAL_ConstructEx(gcvNULL, os, &hal));

	memset(&iface, 0, sizeof(iface));
	iface.command = gcvHAL_VERSION;
	CHECK("gcoHAL_Call(VERSION)", gcoHAL_Call(hal, &iface));
	printf("kernel HAL version %d.%d.%d build %u\n", iface.u.Version.major,
	       iface.u.Version.minor, iface.u.Version.patch, (unsigned)iface.u.Version.build);
	if (iface.u.Version.major != gcvVERSION_MAJOR || iface.u.Version.minor != gcvVERSION_MINOR ||
	    iface.u.Version.patch != gcvVERSION_PATCH || iface.u.Version.build != gcvVERSION_BUILD) {
		printf("RESULT FAIL version mismatch\n");
		return 1;
	}

	CHECK("gcoHAL_QueryChipIdentity",
	      gcoHAL_QueryChipIdentity(hal, &model, &revision, &features, &minor));
	printf("chip model 0x%x revision 0x%x features 0x%08x minor 0x%08x\n",
	       (unsigned)model, (unsigned)revision, (unsigned)features, (unsigned)minor);
	CHECK("gcoHAL_QueryVideoMemory",
	      gcoHAL_QueryVideoMemory(hal, &ia, &is, &ea, &es, &ca, &cs));
	printf("video memory internal %zu external %zu contiguous %zu bytes\n",
	       (size_t)is, (size_t)es, (size_t)cs);

	gcoHAL_DestroyEx(hal);
	gcoOS_Destroy(os);
	printf("RESULT PASS user and kernel HAL versions match\n");
	return 0;
}
