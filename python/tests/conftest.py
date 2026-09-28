import os
import sys

# 发布布局：python/tests/ → python/ 进入 sys.path，直接导入 topos_codec 包
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
