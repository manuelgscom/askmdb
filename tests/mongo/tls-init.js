// Init script of the mongo-tls service (/docker-entrypoint-initdb.d, runs once as root on an
// empty data directory): loads the humo seed and creates the read-only test user.
// Toy credentials of a throwaway container, not a secret.

load("/seed/humo.js");

db.getSiblingDB("humo").createUser({
  user: "lector",
  pwd: "lector-juguete",
  roles: [{ role: "read", db: "humo" }],
  mechanisms: ["SCRAM-SHA-256"],
});
print("tls-init: user lector (read on humo) created");
