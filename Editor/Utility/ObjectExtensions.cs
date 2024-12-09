using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;

namespace SailorEditor.Utility
{
    public static class ObjectExtensions
    {
        public static void CopyPropertiesFrom(this object target, object source)
        {
            var targetType = target.GetType();
            var sourceType = source.GetType();

            foreach (var sourceProp in sourceType.GetProperties(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                var targetProp = targetType.GetProperty(sourceProp.Name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                if (targetProp != null && targetProp.CanWrite)
                {
                    var value = sourceProp.GetValue(source, null);

                    if (value != targetProp.GetValue(target, null))
                    {
                        targetProp.SetValue(target, value, null);
                    }
                }
            }
        }
    }
}
