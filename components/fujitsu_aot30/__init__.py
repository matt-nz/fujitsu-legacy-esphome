import esphome.codegen as cg
from esphome.components import climate

CODEOWNERS = ["@matt-nz"]
DEPENDENCIES = ["climate"]

fujitsu_aot30_ns = cg.esphome_ns.namespace("fujitsu_aot30")
Aot30Climate = fujitsu_aot30_ns.class_("Aot30Climate", climate.Climate, cg.Component)
