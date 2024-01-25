using Editor.Models;
using Editor.Services;

namespace Editor.Helpers
{
    public class FolderTreeViewBuilder
    {
        private TreeViewItemGroup FindParentDepartment(TreeViewItemGroup group, Department department)
        {
            if (group.GroupId == department.ParentDepartmentId)
                return group;

            if (group.Children != null)
            {
                foreach (var currentGroup in group.Children)
                {
                    var search = FindParentDepartment(currentGroup, department);

                    if (search != null)
                        return search;
                }
            }

            return null;
        }

        public TreeViewItemGroup GroupData(AssetsService service)
        {
            var company = service.GetCompany();
            var departments = service.GetDepartments().OrderBy(x => x.ParentDepartmentId);
            var employees = service.GetEmployees();

            var companyGroup = new TreeViewItemGroup();
            companyGroup.Name = company.CompanyName;

            foreach (var dept in departments)
            {
                var itemGroup = new TreeViewItemGroup();
                itemGroup.Name = dept.DepartmentName;
                itemGroup.GroupId = dept.DepartmentId;

                // Employees first
                var employeesDepartment = employees.Where(x => x.DepartmentId == dept.DepartmentId);

                foreach (var emp in employeesDepartment)
                {
                    var item = new TreeViewItem();
                    item.ItemId = emp.EmployeeId;
                    item.Key = emp.EmployeeName;

                    itemGroup.XamlItems.Add(item);
                }

                // Departments now
                if (dept.ParentDepartmentId == -1)
                {
                    companyGroup.Children.Add(itemGroup);
                }
                else
                {
                    TreeViewItemGroup parentGroup = null;

                    foreach (var group in companyGroup.Children)
                    {
                        parentGroup = FindParentDepartment(group, dept);

                        if (parentGroup != null)
                        {
                            parentGroup.Children.Add(itemGroup);
                            break;
                        }
                    }
                }
            }

            return companyGroup;
        }
    }
}