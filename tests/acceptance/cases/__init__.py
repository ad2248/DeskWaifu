"""验收用例注册表：新增用例时导入对应模块并加入 ``MODULES``。"""
from . import gui_panel_open_close

MODULES = {module.CASE.id: module for module in (gui_panel_open_close,)}
CASES = tuple(module.CASE for module in MODULES.values())
