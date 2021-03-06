BEGIN {
    printf("/* Generated by: awk -f usage_page.awk usb_hid_usages */\n\n");
    printf("const char *usage_page(int i);\n\n");
    printf("const char *\n");
    printf("usage_page(int i)\n");
    printf("{\n");
    printf("\tswitch(i) {\n");
}

/^[0-9]/{
    printf("\tcase %s:\n", $1);
    printf("\t\treturn(\"%s\");\n", substr($0, index($0, $2), length));
}

END {
    printf("\tdefault:\n");
    printf("\t\treturn(\"Unknown Page\");\n");
    printf("\t}\n");
    printf("}\n");
}
