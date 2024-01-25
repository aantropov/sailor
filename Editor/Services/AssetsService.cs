using Editor.Models;

namespace Editor.Services
{
    public class AssetsService
    {
        public Company GetCompany()
        {
            return new Company()
            {
                CompanyId = 1,
                CompanyName = "TC Solutions"
            };
        }

        public IEnumerable<Department> GetDepartments()
        {
            return new List<Department>()
            {
                new Department() { CompanyId = 1, DepartmentId = 1, DepartmentName = "IT", ParentDepartmentId = -1 },
                new Department() { CompanyId = 1,  DepartmentId = 2, DepartmentName = "Accounting", ParentDepartmentId = -1 },
                new Department() { CompanyId = 1,  DepartmentId = 3, DepartmentName = "Production", ParentDepartmentId = -1 },
                new Department() { CompanyId = 1,  DepartmentId = 4, DepartmentName = "Software", ParentDepartmentId = 1 },
                new Department() { CompanyId = 1,  DepartmentId = 5, DepartmentName = "Support", ParentDepartmentId = 1 },
                new Department() { CompanyId = 1,  DepartmentId = 6, DepartmentName = "Testing", ParentDepartmentId = 4 },
                new Department() { CompanyId = 1,  DepartmentId = 7, DepartmentName = "Accounts receivable", ParentDepartmentId = 2 },
                new Department() { CompanyId = 1,  DepartmentId = 8, DepartmentName = "Accounts payable", ParentDepartmentId = 2 },
                new Department() { CompanyId = 1,  DepartmentId = 9, DepartmentName = "Customers and services", ParentDepartmentId = 8 }
            };
        }

        public IEnumerable<Employee> GetEmployees()
        {
            return new List<Employee>()
            {
                new Employee() { EmployeeId = 1, EmployeeName = "Luis", DepartmentId = 1 },
                new Employee() { EmployeeId = 2, EmployeeName = "Pepe", DepartmentId = 1 },
                new Employee() { EmployeeId = 3, EmployeeName = "Juan", DepartmentId = 2 },
                new Employee() { EmployeeId = 4, EmployeeName = "Inés", DepartmentId = 3 },
                new Employee() { EmployeeId = 5, EmployeeName = "Sara", DepartmentId = 3 },
                new Employee() { EmployeeId = 6, EmployeeName = "Sofy", DepartmentId = 4 },
                new Employee() { EmployeeId = 7, EmployeeName = "Hugo", DepartmentId = 5 },
                new Employee() { EmployeeId = 8, EmployeeName = "Gema", DepartmentId = 5 },
                new Employee() { EmployeeId = 9, EmployeeName = "Olga", DepartmentId = 6 },
                new Employee() { EmployeeId = 1, EmployeeName = "Otto", DepartmentId = 6 },
                new Employee() { EmployeeId = 2, EmployeeName = "Axel", DepartmentId = 6 },
                new Employee() { EmployeeId = 3, EmployeeName = "Eloy", DepartmentId = 7 },
                new Employee() { EmployeeId = 4, EmployeeName = "Flor", DepartmentId = 8 },
                new Employee() { EmployeeId = 5, EmployeeName = "Aída", DepartmentId = 9 },
                new Employee() { EmployeeId = 6, EmployeeName = "Ruth", DepartmentId = 9 }
            };
        }
    }
}
