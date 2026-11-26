.section .rodata.demo_wap,"a",%progbits
.global demo_manifest, demo_manifest_end
demo_manifest:
.incbin "examples/demo/app.json"
demo_manifest_end:
.global demo_html, demo_html_end
demo_html:
.incbin "examples/demo/app.html"
demo_html_end:
.global demo_node, demo_node_end
demo_node:
.incbin "examples/demo/app.js"
demo_node_end:
