{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* vip binding slots (defined by the generated vip.cpp, filled by the plugify module from
           the vip plugin's exported methods) */
        __vip_*;

        /* No exported methods: vip_btw exposes nothing to other plugins. */

    local: *;
};
