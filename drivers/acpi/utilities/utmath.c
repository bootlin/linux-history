/*******************************************************************************
 *
 * Module Name: utmath - Integer math support routines
 *
 ******************************************************************************/

/*
 *  Copyright (C) 2000 - 2002, R. Byron Moore
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include "acpi.h"


#define _COMPONENT          ACPI_UTILITIES
	 ACPI_MODULE_NAME    ("utmath")

/*
 * Support for double-precision integer divide.  This code is included here
 * in order to support kernel environments where the double-precision math
 * library is not available.
 */

#ifndef ACPI_USE_NATIVE_DIVIDE
/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_short_divide
 *
 * PARAMETERS:  in_dividend         - Pointer to the dividend
 *              Divisor             - 32-bit divisor
 *              out_quotient        - Pointer to where the quotient is returned
 *              out_remainder       - Pointer to where the remainder is returned
 *
 * RETURN:      Status (Checks for divide-by-zero)
 *
 * DESCRIPTION: Perform a short (maximum 64 bits divided by 32 bits)
 *              divide and modulo.  The result is a 64-bit quotient and a
 *              32-bit remainder.
 *
 ******************************************************************************/

acpi_status
acpi_ut_short_divide (
	acpi_integer            *in_dividend,
	u32                     divisor,
	acpi_integer            *out_quotient,
	u32                     *out_remainder)
{
	uint64_overlay          dividend;
	uint64_overlay          quotient;
	u32                     remainder32;


	ACPI_FUNCTION_TRACE ("ut_short_divide");

	dividend.full = *in_dividend;

	/* Always check for a zero divisor */

	if (divisor == 0) {
		ACPI_REPORT_ERROR (("acpi_ut_short_divide: Divide by zero\n"));
		return_ACPI_STATUS (AE_AML_DIVIDE_BY_ZERO);
	}

	/*
	 * The quotient is 64 bits, the remainder is always 32 bits,
	 * and is generated by the second divide.
	 */
	ACPI_DIV_64_BY_32 (0, dividend.part.hi, divisor,
			  quotient.part.hi, remainder32);
	ACPI_DIV_64_BY_32 (remainder32, dividend.part.lo,  divisor,
			  quotient.part.lo, remainder32);

	/* Return only what was requested */

	if (out_quotient) {
		*out_quotient = quotient.full;
	}
	if (out_remainder) {
		*out_remainder = remainder32;
	}

	return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_divide
 *
 * PARAMETERS:  in_dividend         - Pointer to the dividend
 *              in_divisor          - Pointer to the divisor
 *              out_quotient        - Pointer to where the quotient is returned
 *              out_remainder       - Pointer to where the remainder is returned
 *
 * RETURN:      Status (Checks for divide-by-zero)
 *
 * DESCRIPTION: Perform a divide and modulo.
 *
 ******************************************************************************/

acpi_status
acpi_ut_divide (
	acpi_integer            *in_dividend,
	acpi_integer            *in_divisor,
	acpi_integer            *out_quotient,
	acpi_integer            *out_remainder)
{
	uint64_overlay          dividend;
	uint64_overlay          divisor;
	uint64_overlay          quotient;
	uint64_overlay          remainder;
	uint64_overlay          normalized_dividend;
	uint64_overlay          normalized_divisor;
	u32                     partial1;
	uint64_overlay          partial2;
	uint64_overlay          partial3;


	ACPI_FUNCTION_TRACE ("ut_divide");


	/* Always check for a zero divisor */

	if (*in_divisor == 0) {
		ACPI_REPORT_ERROR (("acpi_ut_divide: Divide by zero\n"));
		return_ACPI_STATUS (AE_AML_DIVIDE_BY_ZERO);
	}

	divisor.full  = *in_divisor;
	dividend.full = *in_dividend;
	if (divisor.part.hi == 0) {
		/*
		 * 1) Simplest case is where the divisor is 32 bits, we can
		 * just do two divides
		 */
		remainder.part.hi = 0;

		/*
		 * The quotient is 64 bits, the remainder is always 32 bits,
		 * and is generated by the second divide.
		 */
		ACPI_DIV_64_BY_32 (0, dividend.part.hi, divisor.part.lo,
				  quotient.part.hi, partial1);
		ACPI_DIV_64_BY_32 (partial1, dividend.part.lo, divisor.part.lo,
				  quotient.part.lo, remainder.part.lo);
	}

	else {
		/*
		 * 2) The general case where the divisor is a full 64 bits
		 * is more difficult
		 */
		quotient.part.hi   = 0;
		normalized_dividend = dividend;
		normalized_divisor = divisor;

		/* Normalize the operands (shift until the divisor is < 32 bits) */

		do {
			ACPI_SHIFT_RIGHT_64 (normalized_divisor.part.hi,
					 normalized_divisor.part.lo);
			ACPI_SHIFT_RIGHT_64 (normalized_dividend.part.hi,
					 normalized_dividend.part.lo);

		} while (normalized_divisor.part.hi != 0);

		/* Partial divide */

		ACPI_DIV_64_BY_32 (normalized_dividend.part.hi,
				  normalized_dividend.part.lo,
				  normalized_divisor.part.lo,
				  quotient.part.lo, partial1);

		/*
		 * The quotient is always 32 bits, and simply requires adjustment.
		 * The 64-bit remainder must be generated.
		 */
		partial1      = quotient.part.lo * divisor.part.hi;
		partial2.full = (acpi_integer) quotient.part.lo * divisor.part.lo;
		partial3.full = (acpi_integer) partial2.part.hi + partial1;

		remainder.part.hi = partial3.part.lo;
		remainder.part.lo = partial2.part.lo;

		if (partial3.part.hi == 0) {
			if (partial3.part.lo >= dividend.part.hi) {
				if (partial3.part.lo == dividend.part.hi) {
					if (partial2.part.lo > dividend.part.lo) {
						quotient.part.lo--;
						remainder.full -= divisor.full;
					}
				}
				else {
					quotient.part.lo--;
					remainder.full -= divisor.full;
				}
			}

			remainder.full    = remainder.full - dividend.full;
			remainder.part.hi = (u32) -((s32) remainder.part.hi);
			remainder.part.lo = (u32) -((s32) remainder.part.lo);

			if (remainder.part.lo) {
				remainder.part.hi--;
			}
		}
	}

	/* Return only what was requested */

	if (out_quotient) {
		*out_quotient = quotient.full;
	}
	if (out_remainder) {
		*out_remainder = remainder.full;
	}

	return_ACPI_STATUS (AE_OK);
}

#else

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_short_divide, acpi_ut_divide
 *
 * DESCRIPTION: Native versions of the ut_divide functions. Use these if either
 *              1) The target is a 64-bit platform and therefore 64-bit
 *                 integer math is supported directly by the machine.
 *              2) The target is a 32-bit or 16-bit platform, and the
 *                 double-precision integer math library is available to
 *                 perform the divide.
 *
 ******************************************************************************/

acpi_status
acpi_ut_short_divide (
	acpi_integer            *in_dividend,
	u32                     divisor,
	acpi_integer            *out_quotient,
	u32                     *out_remainder)
{

	ACPI_FUNCTION_TRACE ("ut_short_divide");


	/* Always check for a zero divisor */

	if (divisor == 0) {
		ACPI_REPORT_ERROR (("acpi_ut_short_divide: Divide by zero\n"));
		return_ACPI_STATUS (AE_AML_DIVIDE_BY_ZERO);
	}

	/* Return only what was requested */

	if (out_quotient) {
		*out_quotient = *in_dividend / divisor;
	}
	if (out_remainder) {
		*out_remainder = (u32) *in_dividend % divisor;
	}

	return_ACPI_STATUS (AE_OK);
}

acpi_status
acpi_ut_divide (
	acpi_integer            *in_dividend,
	acpi_integer            *in_divisor,
	acpi_integer            *out_quotient,
	acpi_integer            *out_remainder)
{
	ACPI_FUNCTION_TRACE ("ut_divide");


	/* Always check for a zero divisor */

	if (*in_divisor == 0) {
		ACPI_REPORT_ERROR (("acpi_ut_divide: Divide by zero\n"));
		return_ACPI_STATUS (AE_AML_DIVIDE_BY_ZERO);
	}


	/* Return only what was requested */

	if (out_quotient) {
		*out_quotient = *in_dividend / *in_divisor;
	}
	if (out_remainder) {
		*out_remainder = *in_dividend % *in_divisor;
	}

	return_ACPI_STATUS (AE_OK);
}

#endif


