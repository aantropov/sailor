using System.Collections.Specialized;
using System.Reflection;
using System.Runtime.CompilerServices;
using Microsoft.Maui.Handlers;

namespace SailorEditor.Platforms.Linux;

internal static class GtkGestures
{
    static readonly ConditionalWeakTable<View, Binding> bindings = new();
    static readonly MethodInfo sendTapped = typeof(TapGestureRecognizer).GetMethod(
        "SendTapped", BindingFlags.Instance | BindingFlags.NonPublic)
        ?? throw new MissingMethodException(typeof(TapGestureRecognizer).FullName, "SendTapped");

    public static void Configure()
    {
        // The pinned GTK backend executes tap commands but omits Tapped events
        // and click counts. Use MAUI's dispatch entry point for the shared UI.
        ViewHandler.ViewMapper.ModifyMapping(nameof(View.GestureRecognizers), (handler, view, _) =>
        {
            if (view is View control && handler.PlatformView is Gtk.Widget widget)
            {
                bindings.GetValue(control, static view => new Binding(view)).Attach(widget);
            }
        });
    }

    sealed class Binding
    {
        readonly View view;
        readonly List<Gtk.EventController> controllers = [];
        Gtk.Widget? widget;
        int panId;

        public Binding(View view)
        {
            this.view = view;
            if (view.GestureRecognizers is INotifyCollectionChanged changes)
            {
                changes.CollectionChanged += (_, _) => Rebuild();
            }
            view.HandlerChanging += (_, args) =>
            {
                if (args.OldHandler is not null)
                {
                    Clear();
                    widget = null;
                }
            };
        }

        public void Attach(Gtk.Widget target)
        {
            Clear();
            widget = target;
            Rebuild();
        }

        void Clear()
        {
            foreach (var controller in controllers)
            {
                widget?.RemoveController(controller);
                controller.Dispose();
            }
            controllers.Clear();
        }

        void Rebuild()
        {
            Clear();
            if (widget is null)
            {
                return;
            }
            foreach (var recognizer in view.GestureRecognizers)
            {
                if (recognizer is TapGestureRecognizer tap)
                {
                    var click = Gtk.GestureClick.New();
                    click.SetButton(tap.Buttons switch
                    {
                        ButtonsMask.Primary => 1u,
                        ButtonsMask.Secondary => 3u,
                        _ => 0u
                    });
                    click.OnReleased += (_, args) =>
                    {
                        if (args.NPress != tap.NumberOfTapsRequired)
                        {
                            return;
                        }
                        Func<Microsoft.Maui.IElement?, Point?> position = element =>
                            element is null || ReferenceEquals(element, view) ? new Point(args.X, args.Y) : null;
                        sendTapped.Invoke(tap, [view, position]);
                    };
                    Add(click);
                }
                else if (recognizer is PanGestureRecognizer pan)
                {
                    var drag = Gtk.GestureDrag.New();
                    var controller = (IPanGestureController)pan;
                    drag.OnDragBegin += (_, _) => controller.SendPanStarted(view, ++panId);
                    drag.OnDragUpdate += (_, args) => controller.SendPan(view, args.OffsetX, args.OffsetY, panId);
                    drag.OnDragEnd += (_, _) => controller.SendPanCompleted(view, panId);
                    drag.OnCancel += (_, _) => controller.SendPanCanceled(view, panId);
                    Add(drag);
                }
            }
        }

        void Add(Gtk.EventController controller)
        {
            controllers.Add(controller);
            widget!.AddController(controller);
        }
    }
}
