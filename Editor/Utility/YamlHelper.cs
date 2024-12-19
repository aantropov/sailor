using Microsoft.UI.Windowing;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;
using Window = Microsoft.Maui.Controls.Window;

namespace SailorEditor.Helpers
{
    static class YamlHelper
    {
        public static void EmitNode(IEmitter emitter, YamlNode node)
        {
            switch (node)
            {
                case YamlScalarNode scalar:
                    emitter.Emit(new Scalar(null, null, scalar.Value, ScalarStyle.Any, true, false));
                    break;
                case YamlMappingNode mapping:
                    emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));
                    foreach (var kvp in mapping.Children)
                    {
                        EmitNode(emitter, kvp.Key);
                        EmitNode(emitter, kvp.Value);
                    }
                    emitter.Emit(new MappingEnd());
                    break;
                case YamlSequenceNode sequence:
                    emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
                    foreach (var child in sequence.Children)
                    {
                        EmitNode(emitter, child);
                    }
                    emitter.Emit(new SequenceEnd());
                    break;
            }
        }
    };
}