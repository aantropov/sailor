using Microsoft.UI.Windowing;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;
using System.Collections.Generic;
using System.Globalization;
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

        public static List<float> ParseFloatSequence(IParser parser)
        {
            var list = new List<float>();
            parser.Consume<SequenceStart>();
            while (parser.Current is not SequenceEnd)
            {
                if (parser.Current is Scalar scalar)
                {
                    list.Add(float.Parse(scalar.Value, CultureInfo.InvariantCulture.NumberFormat));
                }
                parser.MoveNext();
            }
            parser.Consume<SequenceEnd>();
            return list;
        }

        public static void EmitFloatSequence(IEmitter emitter, IEnumerable<float> values)
        {
            emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
            foreach (var v in values)
            {
                emitter.Emit(new Scalar(null, v.ToString(CultureInfo.InvariantCulture)));
            }
            emitter.Emit(new SequenceEnd());
        }
    };
}