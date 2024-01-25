namespace Editor.Models
{
    public class Department
    {
        public int DepartmentId { get; set; }
        public string DepartmentName { get; set; }
        public int ParentDepartmentId { get; set; }
        public int CompanyId { get; set; }
    }
}
