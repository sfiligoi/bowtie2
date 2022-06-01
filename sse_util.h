/*
 * Copyright 2011, Ben Langmead <langmea@cs.jhu.edu>
 *
 * This file is part of Bowtie 2.
 *
 * Bowtie 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bowtie 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bowtie 2.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SSE_UTIL_H_
#define SSE_UTIL_H_

#include "vec_util.h"
#include "sse_wrap.h"

class SSEm128i {
public:
        typedef __m128i VecT;

        /* Required memory alignment */
        static constexpr size_t AlignBytes = 16;
        /* 8-bit words per vector element */
        static constexpr size_t U8Num = 16;
};

typedef EList_vec<SSEm128i>       EList_m128i;
typedef VECCheckpointer<SSEm128i> SSECheckpointer;

// temp workaround
typedef SSECheckpointer Checkpointer;

#endif
