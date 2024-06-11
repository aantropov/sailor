using SailorEditor.Controls;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.ViewModels;

namespace SailorEditor.Views
{
    public partial class HierarchyView : ContentView
    {
        WorldService service;

        public HierarchyView()
        {
            InitializeComponent();

            this.service = MauiProgram.GetService<WorldService>();

            service.OnUpdateWorldAction += PopulateHierarchyView;
            HierarchyTree.SelectedItemChanged += OnSelectTreeViewNode;
        }

        public static void OnSelectTreeViewNode(object sender, EventArgs args)
        {
            var selectionChanged = args as TreeView.OnSelectItemEventArgs;
            if (selectionChanged != null)
            {
                var selectionService = MauiProgram.GetService<SelectionService>();

                switch (selectionChanged.Model)
                {
                    case TreeViewItem<Component> component:
                        selectionService.SelectObject(component.Model);
                        break;

                    case TreeViewItemGroup<GameObject, Component> gameObject:
                        selectionService.SelectObject(gameObject.Model);
                        break;
                }
            }
        }

        private void PopulateHierarchyView(World world)
        {
            var gameObjectsModel = HierarchyTreeViewBuilder.PopulateWorld(service);
            var rootNodes = Controls.TreeView.PopulateGroup(gameObjectsModel, new TreeViewPopulateArgs() { ItemImage = "blue_document_attribute_c.png", GroupImage = "blue_document.png" });
            HierarchyTree.RootNodes = rootNodes;
        }
    }
}