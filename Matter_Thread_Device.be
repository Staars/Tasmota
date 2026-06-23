#------------------------------------------------------------------------------
# Matter Thread Device - app-specific demo subclass
#
# All generic Matter-over-Thread behavior lives in `matter.Device_Thread`
# (lib/libesp32/berry_matter/src/embedded/Matter_zz_Device_Thread.be) and the
# BLE commissioning protocol stack is `matter.Device_BLE` + `matter.BTP`.
#
# This file only carries the demo light feedback used while commissioning a
# dev board, plus the bare import OT that drives the Thread-radio state
# machine at runtime.
#------------------------------------------------------------------------------#
import OT
import matter

class MATTER_THREAD : matter.Device_Thread
    var have_light

    def init_light()
        try
            import light
            light.set({'bri': 5, 'hue': 150, 'power': true, 'sat': 255})
            self.have_light = (light.get() != nil)
        except ..
            self.have_light = false
        end
    end

    def heart_beat()
        if self.commissioning.is_commissioning_open() == false  return end
        if self.have_light
            import light
            var l = light.get()
            var hue = (l['hue'] + 1) % 256
            light.set({'bri': l['bri'] == 50 ? 5 : 50, 'hue': hue})
        end
    end

    def on_commissioning_success(ctx)
        if self.have_light
            import light
            light.set({'bri':50,'hue':120})    # green
        end
    end

    def on_commissioning_failure(ctx)
        if self.have_light
            import light
            light.set({'bri':50,'hue':0})       # red
        end
    end
end

return MATTER_THREAD()
