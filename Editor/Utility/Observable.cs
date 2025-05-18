﻿using CommunityToolkit.Mvvm.ComponentModel;
using SailorEngine;
using System;
using System.ComponentModel;
using System.Globalization;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEditor.Utility;

public partial class Observable<T> : ObservableObject, ICloneable
    where T : IComparable<T>
{
    public Observable(T v)
    {
        Value = v;
    }

    public static implicit operator Observable<T>(T value) => new Observable<T>(value);
    public static implicit operator T(Observable<T> observable) => observable.Value;
    public object Clone() => new Observable<T>(Value);
    public override string ToString() => Value.ToString();
    public override bool Equals(object obj)
    {
        if (obj == this)
            return true;

        if (obj is Observable<T> other)
            return other.Value != null ? Value?.CompareTo(other.Value) == 0 : (Value == null && other.Value == null);

        return false;
    }
    public override int GetHashCode() => Value?.GetHashCode() ?? 0;

    [ObservableProperty]
    private T value;
}

public class ObservableObjectYamlConverter<T> : IYamlTypeConverter
    where T : IComparable<T>
{
    public bool Accepts(Type type) => type == typeof(Observable<T>);
    public object ReadYaml(IParser parser, Type type)
    {
        var deserializer = SerializationUtils.CreateDeserializerBuilder().Build();

        var t = deserializer.Deserialize<T>(parser);
        return new Observable<T>(t);
    }

    public void WriteYaml(IEmitter emitter, object value, Type type)
    {
        var observableValue = (Observable<T>)value;
        var serializer = SerializationUtils.CreateSerializerBuilder().Build();

        serializer.Serialize(emitter, observableValue.Value);
    }
}