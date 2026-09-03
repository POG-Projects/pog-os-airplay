.section .rodata.pogwake, "a", @progbits
.balign 16
.global pogwake_jarvis_model
pogwake_jarvis_model:
.incbin POG_WAKE_JARVIS
.global pogwake_jarvis_model_end
pogwake_jarvis_model_end:
.balign 16
.global pogwake_nabu_model
pogwake_nabu_model:
.incbin POG_WAKE_NABU
.global pogwake_nabu_model_end
pogwake_nabu_model_end:
.balign 16
.global pogwake_alexa_model
pogwake_alexa_model:
.incbin POG_WAKE_ALEXA
.global pogwake_alexa_model_end
pogwake_alexa_model_end:
