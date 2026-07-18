library(ggplot2)
p <- ggplot(mtcars, aes(hp, mpg, colour = factor(cyl))) +
  geom_point() +
  labs(title = "Fuel efficiency vs horsepower", x = "hp", y = "mpg", colour = "cyl")
ggsave("mtcars_r.pdf", p, width = 6, height = 4)
