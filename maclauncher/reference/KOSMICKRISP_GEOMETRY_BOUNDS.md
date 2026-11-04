# Generated geometry bounds

Generated geometry raster shaders check the decoded output vertex, input primitive and instance against their draw dimensions before reading stage-input buffers or writing transform feedback. Restart values and unused entries in the virtual vertex-ID encoding produce clipped output with zeroed varyings.

Valid generated vertices retain their original position, varyings and transform-feedback behavior. The same guard applies to geometry shaders fed by vertex or tessellation-evaluation shaders.
