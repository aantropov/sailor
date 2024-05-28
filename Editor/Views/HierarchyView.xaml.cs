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

            PopulateHierarchyView();

            HierarchyTree.SelectedItemChanged += OnSelectTreeViewNode;
        }

        public static void OnSelectTreeViewNode(object sender, EventArgs args)
        {
            var selectionChanged = args as TreeView.OnSelectItemEventArgs;
            if (selectionChanged.Model is TreeViewItem<Component> component)
            {
                MauiProgram.GetService<SelectionService>().OnSelect(component.Model);
            }

            if (selectionChanged.Model is TreeViewItem<Component> gameObject)
            {
                MauiProgram.GetService<SelectionService>().OnSelect(gameObject.Model);
            }
        }

        private void PopulateHierarchyView()
        {
            var foldersModel = HierarchyTreeViewBuilder.PopulateWorld(service);
            var rootNodes = Controls.TreeView.PopulateGroup(foldersModel, new TreeViewPopulateArgs() { ItemImage = "blue_document_attribute_c.png", GroupImage = "blue_document.png" });
            HierarchyTree.RootNodes = rootNodes;
        }
    }
}