try:
    import torch as _torch
except ImportError:
    _torch = None

import sys
from types import ModuleType


class _Wrapper(ModuleType):
    def __getattr__(self, name):
        if _torch is None:
            raise ImportError("Triton requires PyTorch to be installed")
        return getattr(_torch, name)


sys.modules[__name__] = _Wrapper(__name__)
