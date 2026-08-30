using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;
using System.Collections.Generic;
using System.Globalization;

namespace SailorEditor.Helpers
{
    internal static class YamlHelper
    {
        public static bool TryGetMapping(
            YamlMappingNode parent,
            string key,
            out YamlMappingNode mapping) =>
            TryGetNode(parent, key, out mapping);

        public static bool TryGetSequence(
            YamlMappingNode parent,
            string key,
            out YamlSequenceNode sequence) =>
            TryGetNode(parent, key, out sequence);

        public static bool TryGetScalar(
            YamlMappingNode parent,
            string key,
            out string value,
            bool requireNonWhitespace = false)
        {
            value = string.Empty;
            if (!TryGetNode<YamlScalarNode>(parent, key, out var scalar) ||
                scalar.Value is null)
            {
                return false;
            }

            value = scalar.Value;
            return !requireNonWhitespace || !string.IsNullOrWhiteSpace(value);
        }

        public static string ReadString(
            YamlMappingNode parent,
            string key,
            string fallback = "") =>
            TryGetScalar(parent, key, out var value) ? value : fallback;

        public static ulong ReadUInt64(
            YamlMappingNode parent,
            string key,
            ulong fallback = 0) =>
            ulong.TryParse(
                ReadString(parent, key),
                NumberStyles.Integer,
                CultureInfo.InvariantCulture,
                out var value)
                ? value
                : fallback;

        public static int ReadInt(
            YamlMappingNode parent,
            string key,
            int fallback = 0) =>
            int.TryParse(
                ReadString(parent, key),
                NumberStyles.Integer,
                CultureInfo.InvariantCulture,
                out var value)
                ? value
                : fallback;

        public static float ReadFloat(
            YamlMappingNode parent,
            string key,
            float fallback = 0.0f) =>
            float.TryParse(
                ReadString(parent, key),
                NumberStyles.Float,
                CultureInfo.InvariantCulture,
                out var value)
                ? value
                : fallback;

        public static bool ReadBool(
            YamlMappingNode parent,
            string key,
            bool fallback = false) =>
            bool.TryParse(ReadString(parent, key), out var value)
                ? value
                : fallback;

        public static YamlScalarNode Scalar(ulong value) =>
            new(value.ToString(CultureInfo.InvariantCulture));

        public static YamlScalarNode Scalar(int value) =>
            new(value.ToString(CultureInfo.InvariantCulture));

        public static YamlScalarNode Scalar(float value) =>
            new(value.ToString("R", CultureInfo.InvariantCulture));

        public static YamlScalarNode Scalar(bool value) =>
            new(value ? "true" : "false");

        public static void EmitNode(IEmitter emitter, YamlNode node)
        {
            switch (node)
            {
                case YamlScalarNode scalar:
                    emitter.Emit(new Scalar(null, null, scalar.Value!, ScalarStyle.Any, true, false));
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

        static bool TryGetNode<TNode>(
            YamlMappingNode parent,
            string key,
            out TNode node)
            where TNode : YamlNode
        {
            if (parent.Children.TryGetValue(new YamlScalarNode(key), out var value) &&
                value is TNode resolved)
            {
                node = resolved;
                return true;
            }

            node = null!;
            return false;
        }
    };
}
