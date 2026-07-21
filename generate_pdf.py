# pyrefly: ignore [missing-import]
from pyhtml2pdf import converter
import os

html_path = os.path.abspath(r"e:\user scheduler\User-thread-Scheduler-\project_report.html")
pdf_path = os.path.abspath(r"e:\user scheduler\User-thread-Scheduler-\UThread_Project_Report.pdf")

print(f"Converting: {html_path}")
print(f"Output: {pdf_path}")

converter.convert(
    f'file:///{html_path}',
    pdf_path,
    print_options={
        'landscape': False,
        'displayHeaderFooter': False,
        'printBackground': True,
        'preferCSSPageSize': True,
        'paperWidth': 8.27,
        'paperHeight': 11.69,
        'marginTop': 0,
        'marginBottom': 0,
        'marginLeft': 0,
        'marginRight': 0,
    }
)

print("PDF generated successfully!")
