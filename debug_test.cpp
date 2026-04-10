#include "css_parser.h"
#include "html_parser.h"
#include "layout.h"
#include "paint.h"
#include "style.h"
#include <fstream>
#include <iostream>


int main() {
  std::string html = "<html><body><h1>Hello Engine!</h1><div "
                     "class=\"box\"><p>Paragraph text.</p></div></body></html>";
  std::string css =
      "h1, h2 { color: red; font-size: 20px; } .box { background: blue; }";

  HTMLParser parser(html);
  auto root = parser.parse();

  CSSParser css_parser(css);
  auto stylesheet = css_parser.parse_stylesheet();

  auto styleTreeRoot = style_tree(root, stylesheet);

  auto layoutRoot = build_layout_tree(styleTreeRoot);

  Dimensions viewport;
  viewport.content.width = 800;
  viewport.content.height = 600;

  layoutRoot->layout(viewport);

  auto master_display_list = build_display_list(layoutRoot);

  std::ofstream out("debug.txt");
  out << "Display list size: " << master_display_list.size() << "\n";
  for (const auto &cmd : master_display_list) {
    out << "CMD rect: " << cmd.rect.x << "," << cmd.rect.y << " "
        << cmd.rect.width << "x" << cmd.rect.height << " color " << cmd.color
        << "\n";
  }
  out.close();
  return 0;
}
