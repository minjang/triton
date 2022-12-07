from typing import List

import triton
import triton.language as tl


def _i_arange(
    start: int,
    end: int,
    builder: tl.ir.builder,
) -> tl.tensor:
    if not isinstance(start, int) or not isinstance(end, int):
        raise ValueError("arange's arguments must be of type constexpr")

    shape = [end - start]
    ret_ty = tl.block_type(tl.int32, shape)
    return tl.tensor(builder.create_make_range(start, end), ret_ty)


@triton.builtin
def arange(
    start,
    end,
    _builder=None,
):
    """
    Returns contiguous values within the open interval [:code:`start`, :code:`end`).

    :param start: Start of the interval. Must be a power of two.
    :type start: int
    :param stop: End of the interval. Must be a power of two >= start.
    :type stop: int
    """
    start = tl._constexpr_to_value(start)
    end = tl._constexpr_to_value(end)
    return _i_arange(start, end, _builder)


def _i_zeros(
    shape: List[int],
    dtype: tl.dtype,
    builder: tl.ir.builder,
) -> tl.tensor:
    _0 = builder.get_null_value(dtype.to_ir(builder))
    ret_ty = tl.block_type(dtype, shape)
    return tl.tensor(builder.create_splat(_0, shape), ret_ty)


@triton.builtin
def zeros(
    shape,
    dtype,
    _builder=None,
):
    """
    Returns a tensor filled with the scalar value 0 for the given :code:`shape` and :code:`dtype`.

    :param shape: Shape of the new array, e.g., (8, 16) or (8, )
    :type shape: tuple of ints
    :param dtype: Data-type of the new array, e.g., :code:`tl.float16`
    :type dtype: DType
    """
    for i, d in enumerate(shape):
        if not isinstance(d, tl.constexpr):
            raise TypeError(f"Shape element {i} must have type `constexpr`")
        if not isinstance(d.value, int):
            raise TypeError(
                f"Shape element {i} must have type `constexpr[int]`, got `constexpr[{type(d.value)}]"
            )
    shape = [x.value for x in shape]
    dtype = tl._constexpr_to_value(dtype)
    return _i_zeros(shape, dtype, _builder)


@triton.jit
def zeros_like(input):
    return zeros(input.shape, input.dtype)


def _i_cat(
    lhs: tl.tensor,
    rhs: tl.tensor,
    can_reorder: bool,
    builder: tl.ir.builder,
) -> tl.tensor:
    assert can_reorder, "current implementation of 'cat' always may reorder elements"
    assert len(lhs.shape) == 1
    ret_type = tl.block_type(lhs.type.scalar, [lhs.shape[0] + rhs.shape[0]])
    return tl.tensor(
        builder.create_cat(lhs.handle, rhs.handle),
        ret_type,
    )


@triton.builtin
def cat(input, other, can_reorder=False, _builder=None):
    """
    Concatenate the given blocks

    :param input: The first input tensor.
    :type input:
    :param other: The second input tensor.
    :type other:
    :param can_reorder: compiler hint. If true, the compiler is
    allowed to reorder elements while concatenating inputs,
    Only use if teh order does not matter (e.g., result is
    only used in reduction ops)
    """
    return _i_cat(input, other, can_reorder, _builder)
