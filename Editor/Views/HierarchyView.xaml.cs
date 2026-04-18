using CommunityToolkit.Maui.Markup;
using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Controls;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Collections.ObjectModel;

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
            HierarchyTree.DropRequested += OnHierarchyDropRequested;

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectInstanceAction += SelectInstance;
        }

        private void SelectInstance(InstanceId id)
        {
            if (!id.IsEmpty())
            {
                if (HierarchyTree.SelectedItem.BindingContext is TreeViewItem<Component> component)
                {
                    if (component.Model.InstanceId != id)
                    {
                        foreach (var el in HierarchyTree.RootNodes)
                        {
                            var res = el.FindItemRecursive(id);

                            if (res != null)
                            {
                                res.Select();
                                HierarchyTree.SelectedItem = res;
                                break;
                            }
                        }
                    }
                }
            }
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

        private void OnHierarchyDropRequested(object sender, Utility.TreeViewDropRequest request)
        {
            if (request.Target is null)
            {
                request.Handled = request.Source switch
                {
                    GameObject source => service.ReparentToRoot(source, keepWorldTransform: true),
                    PrefabFile prefab => MauiProgram.GetService<EngineService>().InstantiatePrefab(prefab.FileId),
                    _ => false
                };
                return;
            }

            if (request.Target is not GameObject target)
            {
                return;
            }

            request.Handled = request.Source switch
            {
                GameObject source => service.Reparent(source, target, keepWorldTransform: true),
                PrefabFile prefab => MauiProgram.GetService<EngineService>().InstantiatePrefab(prefab.FileId, target.InstanceId),
                _ => false
            };
        }

        private void PopulateHierarchyView(World world)
        {
            var gameObjectsModel = HierarchyTreeViewBuilder.PopulateWorld(service);
            var rootNodes = Controls.TreeView.PopulateGroup(gameObjectsModel, new TreeViewPopulateArgs()
            {
                ItemImage = "blue_document_attribute_c.png",
                GroupImage = "blue_document.png",
                ExpandGroupsByDefault = true
            });

            //HierarchyTree.StackLayout.Children.Clear();
            HierarchyTree.RootNodes = rootNodes;
        }
    }
}
