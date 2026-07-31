.section .rodata.web_index_html, "a", @progbits
.balign 4

.global _binary_index_html_start
.global _binary_index_html_end

_binary_index_html_start:
    .incbin WEB_INDEX_HTML_PATH
    .byte 0
_binary_index_html_end:

.section .rodata.web_logs_html, "a", @progbits
.balign 4

.global _binary_logs_html_start
.global _binary_logs_html_end

_binary_logs_html_start:
    .incbin WEB_LOGS_HTML_PATH
    .byte 0
_binary_logs_html_end:
