struct ds4_metal_args_argsort {
    int32_t  ne00;
    int32_t  ne01;
    int32_t  ne02;
    int32_t  ne03;
    uint64_t nb00;
    uint64_t nb01;
    uint64_t nb02;
    uint64_t nb03;
    int32_t  ne0;
    int32_t  ne1;
    int32_t  ne2;
    int32_t  ne3;
    int32_t  top_k;
};

struct ds4_metal_args_argsort_merge {
    int64_t  ne00;
    int64_t  ne01;
    int64_t  ne02;
    int64_t  ne03;
    uint64_t nb00;
    uint64_t nb01;
    uint64_t nb02;
    uint64_t nb03;
    int32_t  ne0;
    int32_t  ne1;
    int32_t  ne2;
    int32_t  ne3;
    int32_t  top_k;
    int32_t  len;
};

struct ds4_metal_args_top1 {
    int32_t n_comp;
    int32_t n_rows;
};

struct ds4_metal_args_top1_pair {
    int32_t n_items;
    int32_t n_rows;
};

struct ds4_metal_args_top1_pair_embed_norm {
    int32_t  n_items;
    int32_t  n_rows;
    uint32_t n_embd;
    uint32_t n_embd4;
    uint32_t n_vocab;
    uint32_t _pad0;
    uint64_t embed_row_stride;
    float    eps;
};

typedef void (argsort_t)(
        constant   ds4_metal_args_argsort & args,
        device   const char * src0,
        device      int32_t * dst,
        threadgroup int32_t * shmem_i32 [[threadgroup(0)]],
        uint3   tgpig[[threadgroup_position_in_grid]],
        ushort3 tpitg[[thread_position_in_threadgroup]],
        ushort3   ntg[[threads_per_threadgroup]]);

// Sort one float row into an index row. DS4 only exports the descending
// instance because router and indexer selection both need top-k order.
template<ds4_sort_order order>
kernel void kernel_argsort_f32_i32(
        constant   ds4_metal_args_argsort & args,
        device   const char * src0,
        device      int32_t * dst,
        threadgroup int32_t * shmem_i32 [[threadgroup(0)]],
        uint3   tgpig[[threadgroup_position_in_grid]],
        ushort3 tpitg[[thread_position_in_threadgroup]],
        ushort3   ntg[[threads_per_threadgroup]]) {
    // bitonic sort
    const int col = tpitg[0];
    const int ib  = tgpig[0] / args.ne01;

    const int i00 = ib*ntg.x;
    const int i01 = tgpig[0] % args.ne01;
    const int i02 = tgpig[1];
    const int i03 = tgpig[2];

    device const float * src0_row = (device const float *) (src0 + args.nb01*i01 + args.nb02*i02 + args.nb03*i03);

    // initialize indices
    shmem_i32[col] = i00 + col;

    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (int k = 2; k <= ntg.x; k *= 2) {
        for (int j = k / 2; j > 0; j /= 2) {
            int ixj = col ^ j;
            if (ixj > col) {
                if ((col & k) == 0) {
                    if (shmem_i32[col] >= args.ne00 ||
                       (shmem_i32[ixj] <  args.ne00 && (order == DS4_SORT_ORDER_ASC ?
                            src0_row[shmem_i32[col]] > src0_row[shmem_i32[ixj]] :
                            src0_row[shmem_i32[col]] < src0_row[shmem_i32[ixj]]))
                    ) {
                        SWAP(shmem_i32[col], shmem_i32[ixj]);
                    }
                } else {
                    if (shmem_i32[ixj] >= args.ne00 ||
                       (shmem_i32[col] <  args.ne00 && (order == DS4_SORT_ORDER_ASC ?
                            src0_row[shmem_i32[col]] < src0_row[shmem_i32[ixj]] :
                            src0_row[shmem_i32[col]] > src0_row[shmem_i32[ixj]]))
                    ) {
                        SWAP(shmem_i32[col], shmem_i32[ixj]);
                    }
                }
            }

            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
    }

    const int64_t i0 = ib*args.top_k;

    // copy the result to dst without the padding
    if (i0 + col < args.ne0 && col < args.top_k) {
        dst += i0 + args.ne0*i01 + args.ne0*args.ne1*i02 + args.ne0*args.ne1*args.ne2*i03;

        dst[col] = shmem_i32[col];
    }
}

// Host-visible sort variant used by DS4 top-k selection.
template [[host_name("kernel_argsort_f32_i32_desc")]] kernel argsort_t kernel_argsort_f32_i32<DS4_SORT_ORDER_DESC>;

kernel void kernel_top1_f32_i32(
        constant ds4_metal_args_top1 & args,
        device const float *scores,
        device int32_t *dst,
        threadgroup float *shmem_val [[threadgroup(0)]],
        threadgroup int32_t *shmem_idx [[threadgroup(1)]],
        uint row [[threadgroup_position_in_grid]],
        ushort tid [[thread_position_in_threadgroup]],
        ushort ntg [[threads_per_threadgroup]]) {
    if ((int32_t)row >= args.n_rows) return;
    device const float *row_scores = scores + (uint64_t)row * (uint64_t)args.n_comp;
    float best_val = -3.402823466e+38f;
    int32_t best_idx = 0;
    for (int32_t i = (int32_t)tid; i < args.n_comp; i += (int32_t)ntg) {
        const float v = row_scores[i];
        if (v > best_val || (v == best_val && i < best_idx)) {
            best_val = v;
            best_idx = i;
        }
    }
    shmem_val[tid] = best_val;
    shmem_idx[tid] = best_idx;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (ushort stride = ntg >> 1; stride > 0; stride >>= 1) {
        if (tid < stride) {
            const ushort other = tid + stride;
            const float v = shmem_val[other];
            const int32_t idx = shmem_idx[other];
            if (v > shmem_val[tid] || (v == shmem_val[tid] && idx < shmem_idx[tid])) {
                shmem_val[tid] = v;
                shmem_idx[tid] = idx;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    if (tid == 0) dst[row] = shmem_idx[0];
}

kernel void kernel_top1_pair_f32_i32(
        constant ds4_metal_args_top1_pair & args,
        device const float *scores,
        device const int32_t *ids,
        device int32_t *dst,
        threadgroup float *shmem_val [[threadgroup(0)]],
        threadgroup int32_t *shmem_idx [[threadgroup(1)]],
        uint row [[threadgroup_position_in_grid]],
        ushort tid [[thread_position_in_threadgroup]],
        ushort ntg [[threads_per_threadgroup]]) {
    if ((int32_t)row >= args.n_rows) return;
    device const float *row_scores = scores + (uint64_t)row * (uint64_t)args.n_items;
    device const int32_t *row_ids = ids + (uint64_t)row * (uint64_t)args.n_items;
    float best_val = -3.402823466e+38f;
    int32_t best_idx = 2147483647;
    for (int32_t i = (int32_t)tid; i < args.n_items; i += (int32_t)ntg) {
        const float v = row_scores[i];
        const int32_t idx = row_ids[i];
        if (v > best_val || (v == best_val && idx < best_idx)) {
            best_val = v;
            best_idx = idx;
        }
    }
    shmem_val[tid] = best_val;
    shmem_idx[tid] = best_idx;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (ushort stride = ntg >> 1; stride > 0; stride >>= 1) {
        if (tid < stride) {
            const ushort other = tid + stride;
            const float v = shmem_val[other];
            const int32_t idx = shmem_idx[other];
            if (v > shmem_val[tid] || (v == shmem_val[tid] && idx < shmem_idx[tid])) {
                shmem_val[tid] = v;
                shmem_idx[tid] = idx;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    if (tid == 0) dst[row] = shmem_idx[0];
}

kernel void kernel_top1_pair_embed_norm_f16_f32(
        constant ds4_metal_args_top1_pair_embed_norm & args,
        device const float *scores,
        device const int32_t *ids,
        device int32_t *dst_token,
        device const char *embed,
        device const float4 *weight,
        device float4 *dst_norm,
        threadgroup float *shmem_val [[threadgroup(0)]],
        threadgroup int32_t *shmem_idx [[threadgroup(1)]],
        uint row [[threadgroup_position_in_grid]],
        ushort tid [[thread_position_in_threadgroup]],
        ushort ntg [[threads_per_threadgroup]]) {
    if ((int32_t)row >= args.n_rows) return;
    device const float *row_scores = scores + (uint64_t)row * (uint64_t)args.n_items;
    device const int32_t *row_ids = ids + (uint64_t)row * (uint64_t)args.n_items;
    float best_val = -3.402823466e+38f;
    int32_t best_idx = 2147483647;
    for (int32_t i = (int32_t)tid; i < args.n_items; i += (int32_t)ntg) {
        const float v = row_scores[i];
        const int32_t idx = row_ids[i];
        if (v > best_val || (v == best_val && idx < best_idx)) {
            best_val = v;
            best_idx = idx;
        }
    }
    shmem_val[tid] = best_val;
    shmem_idx[tid] = best_idx;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (ushort stride = ntg >> 1; stride > 0; stride >>= 1) {
        if (tid < stride) {
            const ushort other = tid + stride;
            const float v = shmem_val[other];
            const int32_t idx = shmem_idx[other];
            if (v > shmem_val[tid] || (v == shmem_val[tid] && idx < shmem_idx[tid])) {
                shmem_val[tid] = v;
                shmem_idx[tid] = idx;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    const int32_t tok = shmem_idx[0];
    if (tid == 0) dst_token[row] = tok;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (tok < 0 || (uint32_t)tok >= args.n_vocab) return;

    device const half *embed_row =
        (device const half *)(embed + (uint64_t)tok * args.embed_row_stride);
    float sumf = 0.0f;
    for (uint32_t i = (uint32_t)tid; i < args.n_embd4; i += (uint32_t)ntg) {
        const uint32_t off = i * 4u;
        const float4 v = float4(embed_row[off + 0u],
                                embed_row[off + 1u],
                                embed_row[off + 2u],
                                embed_row[off + 3u]);
        sumf += dot(v, v);
    }
    shmem_val[tid] = sumf;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (ushort stride = ntg >> 1; stride > 0; stride >>= 1) {
        if (tid < stride) {
            shmem_val[tid] += shmem_val[tid + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    const float scale = 1.0f / sqrt(shmem_val[0] / (float)args.n_embd + args.eps);
    device float4 *dst_row = dst_norm + (uint64_t)row * (uint64_t)args.n_embd4;
    for (uint32_t i = (uint32_t)tid; i < args.n_embd4; i += (uint32_t)ntg) {
        const uint32_t off = i * 4u;
        const float4 v = float4(embed_row[off + 0u],
                                embed_row[off + 1u],
                                embed_row[off + 2u],
                                embed_row[off + 3u]);
        dst_row[i] = (v * scale) * weight[i];
    }
}

typedef void (argsort_merge_t)(
        constant   ds4_metal_args_argsort_merge & args,
        device const char    * src0,
        device const int32_t * tmp,
        device       int32_t * dst,
        uint3   tgpig[[threadgroup_position_in_grid]],
        ushort3 tpitg[[thread_position_in_threadgroup]],
        ushort3   ntg[[threads_per_threadgroup]]);

// Merges sorted index runs produced by kernel_argsort_f32_i32. In the DS4 graph
// this finishes top-k over router or compressed-attention score rows.
template<ds4_sort_order order>
kernel void kernel_argsort_merge_f32_i32(
        constant   ds4_metal_args_argsort_merge & args,
        device const char    * src0,
        device const int32_t * tmp,
        device       int32_t * dst,
        uint3   tgpig[[threadgroup_position_in_grid]],
        ushort3 tpitg[[thread_position_in_threadgroup]],
        ushort3   ntg[[threads_per_threadgroup]]) {

    const int im  = tgpig[0] / args.ne01;
    const int i01 = tgpig[0] % args.ne01;
    const int i02 = tgpig[1];
    const int i03 = tgpig[2];

    const int start = im * (2 * args.len);

    const int len0 = MIN(args.len, MAX(0, args.ne0 - (int)(start)));
    const int len1 = MIN(args.len, MAX(0, args.ne0 - (int)(start + args.len)));

    const int total = len0 + len1;

    device const int32_t * tmp0 = tmp + start
        + i01*args.ne0
        + i02*args.ne0*args.ne01
        + i03*args.ne0*args.ne01*args.ne02;

    device const int32_t * tmp1 = tmp0 + args.len;

    dst += start
        + i01*args.top_k
        + i02*args.top_k*args.ne01
        + i03*args.top_k*args.ne01*args.ne02;

    device const float * src0_row = (device const float *)(src0
        + args.nb01*i01
        + args.nb02*i02
        + args.nb03*i03);

    if (total == 0) {
        return;
    }

    const int chunk = (total + ntg.x - 1) / ntg.x;

    const int k0 = tpitg.x * chunk;
    const int k1 = MIN(MIN(k0 + chunk, total), args.top_k);

    if (k0 >= args.top_k) {
        return;
    }

    if (k0 >= total) {
        return;
    }

    int low  = k0 > len1 ? k0 - len1 : 0;
    int high = MIN(k0, len0);

    // binary-search partition (i, j) such that i + j = k
    while (low < high) {
        const int mid = (low + high) >> 1;

        const int32_t idx0 = tmp0[mid];
        const int32_t idx1 = tmp1[k0 - mid - 1];

        const float val0 = src0_row[idx0];
        const float val1 = src0_row[idx1];

        bool take_left;
        if (order == DS4_SORT_ORDER_ASC) {
            take_left = (val0 <= val1);
        } else {
            take_left = (val0 >= val1);
        }

        if (take_left) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    int i = low;
    int j = k0 - i;

    // keep the merge fronts into registers
    int32_t idx0 = 0;
    float   val0 = 0.0f;
    if (i < len0) {
        idx0 = tmp0[i];
        val0 = src0_row[idx0];
    }

    int32_t idx1 = 0;
    float   val1 = 0.0f;
    if (j < len1) {
        idx1 = tmp1[j];
        val1 = src0_row[idx1];
    }

    for (int k = k0; k < k1; ++k) {
        int32_t out_idx;

        if (i >= len0) {
            while (k < k1) {
                dst[k++] = tmp1[j++];
            }
            break;
        } else if (j >= len1) {
            while (k < k1) {
                dst[k++] = tmp0[i++];
            }
            break;
        } else {
            bool take_left;

            if (order == DS4_SORT_ORDER_ASC) {
                take_left = (val0 <= val1);
            } else {
                take_left = (val0 >= val1);
            }

            if (take_left) {
                out_idx = idx0;
                ++i;
                if (i < len0) {
                    idx0 = tmp0[i];
                    val0 = src0_row[idx0];
                }
            } else {
                out_idx = idx1;
                ++j;
                if (j < len1) {
                    idx1 = tmp1[j];
                    val1 = src0_row[idx1];
                }
            }
        }

        dst[k] = out_idx;
    }
}

// Host-visible merge variant used by DS4 top-k selection.
template [[host_name("kernel_argsort_merge_f32_i32_desc")]] kernel argsort_merge_t kernel_argsort_merge_f32_i32<DS4_SORT_ORDER_DESC>;
